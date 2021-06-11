#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <dirent.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/shm.h>

#define MM_HUGEPAGES_PATH "/sys/kernel/mm/hugepages"
#define _MAP_HUGE_MASK 0x3f
#define _MAP_HUGE_SHIFT 26

#define IS_POW2(n) ((n) > 0 && ((n) & ((n) - 1)) == 0)
#define IS_DIV2(n) (((n) % 2) == 0)

static std::unique_ptr<size_t> is_hugepage(const std::string &filename) {
	/*
	 * From https://www.kernel.org/doc/Documentation/vm/hugetlbpage.txt:
	 *
	 * > For each huge page size supported by the running kernel, a subdirectory
	 * > will exist, of the form:
	 * >
	 * > hugepages-${size}kB
	 */
	const std::string prefix("hugepages-");
	const auto found = filename.find(prefix);
	if (found == std::string::npos) {
		return nullptr;
	}

	const std::string raw_size = filename.substr(prefix.length(), filename.length() - prefix.length() - 2);
	return std::make_unique<size_t>(std::stol(raw_size) * 1024UL);
}

inline static unsigned short determine_shift(size_t size) {
	unsigned short i = 0;
	while (size >>= 1) {
		i++;
	}
	return i;
}

static void determine_supported_hps(std::vector<std::pair<size_t, unsigned short>> &supported_hps) {
	DIR *dir;
	struct dirent *ent;
	if ((dir = opendir(MM_HUGEPAGES_PATH)) == nullptr) {
		throw std::system_error(errno, std::generic_category(), "opendir " MM_HUGEPAGES_PATH);
	}

	while ((ent = readdir(dir)) != nullptr) {
		char *path = ent->d_name;
		char *filename = basename(path);

		if (const auto hugesize = is_hugepage(filename)) {
			supported_hps.push_back(std::make_pair(*hugesize, determine_shift(*hugesize)));
		}
	}
	closedir(dir);
}

static std::unique_ptr<unsigned short> get_page_size(const std::vector<std::pair<size_t, unsigned short>> &page_sizes, const size_t size) {
	if (!IS_DIV2(size)) {
		return nullptr;
	}

	for (auto it = page_sizes.rbegin(); it != page_sizes.rend(); ++it) {
		auto elem = *it;
		auto hp_size = elem.first;
		if (size >= hp_size && size % hp_size == 0) {
			return std::make_unique<unsigned short>(elem.second);
		}
	}

	return nullptr;
}

static std::pair<bool, size_t> check_available_pages(const size_t page_size, const size_t expected) {
	auto pagesz_kb = page_size / 1024;
	char pathbuf[PATH_MAX];
	snprintf(pathbuf, PATH_MAX-1, MM_HUGEPAGES_PATH "/hugepages-%lukB/free_hugepages", pagesz_kb);

	std::ifstream fhp_file(pathbuf);
	std::string value;
	fhp_file >> value;

	size_t available = std::stol(value);

	return std::make_pair(available >= expected, available);
}

int main(int argc, char **argv) {
	// Figure out supported types
	std::vector<std::pair<size_t, unsigned short>> supported_hps;
	determine_supported_hps(supported_hps);

	// Print out supported page sizes
	fprintf(stderr, "Supported huge page sizes:\n");
	for (auto it = supported_hps.begin(); it != supported_hps.end(); ++it) {
		auto elem = *it;
		fprintf(stderr, "- size=%lu (shift=%u)\n", elem.first, elem.second);
	}

	if (argc != 3) {
		fprintf(stderr, "USAGE: %s <shift> <multiplier>\n", argv[0]);
		return 1;
	}

	auto shiftarg = std::stol(argv[1]);
	auto multiplier = std::stol(argv[2]);
	size_t shift = 0;
	size_t div = 0;

	if (shiftarg == 0 || multiplier == 0) {
		fprintf(stderr, "Neither multiplier nor shift can be < 0\n");
		return 1;
	}

	size_t sz = multiplier * (1 << shiftarg);

	// Check if this is size & shift supported
	if (const auto shift_opt = get_page_size(supported_hps, sz)) {
		shift = *shift_opt;
		div = sz / (1 << shift);
		fprintf(stderr, "size=%lu, shift=%lu, div=%lu\n", sz, shift, div);
	} else {
		fprintf(stderr, "size=%lu, does not fit to any supported size!\n", sz);
		return 1;
	}

	auto page_info = check_available_pages((1 << shift), div);
	if (!page_info.first) {
		fprintf(stderr, "Not enough available pages (need=%lu, free=%lu), allocation will fail very likely\n", div, page_info.second);
	};

	// Check rlimit
	struct rlimit memlock_cur;
	if (getrlimit(RLIMIT_MEMLOCK, &memlock_cur) < 0) {
		throw std::system_error(errno, std::generic_category(), "getrlimit RLIMIT_MEMLOCK");
	}

	// Adjust limits if needed.
	bool memlock_enough = true;
	if (memlock_cur.rlim_max != RLIM_INFINITY && memlock_cur.rlim_max < sz) {
		fprintf(stderr, "RLIM_MEMLOCK hard limit is too small (%lu < %lu)\n", memlock_cur.rlim_max, sz);
		memlock_enough = false;
	}

	if (memlock_cur.rlim_cur < sz && memlock_cur.rlim_max >= sz) {
		fprintf(stderr, "Adjusting RLIM_MEMLOCK soft limit (%lu -> %lu)\n", memlock_cur.rlim_cur, sz);
		memlock_cur.rlim_cur = sz;
		if (setrlimit(RLIMIT_MEMLOCK, &memlock_cur) < 0) {
			throw std::system_error(errno, std::generic_category(), "setrlimit RLIMIT_MEMLOCK");
		}
	}

	// Allocate using shmget
	int flags = SHM_HUGETLB | IPC_CREAT | SHM_R | SHM_W | ((shift & _MAP_HUGE_MASK) << _MAP_HUGE_SHIFT);
	int shmid = shmget(IPC_PRIVATE, sz, flags);
	if (shmid == -1) {
		int err = errno;
		if (err == EPERM && !memlock_enough) {
			fprintf(stderr, "Caught EPERM while shmget(). Check '/proc/sys/vm/hugetlb_shm_group'? (alternative to RLIM_MEMLOCK)\n");
		}
		throw std::system_error(err, std::generic_category(), "shmget");
	}

	char *shmaddr = (char *) shmat(shmid, NULL, 0);
	if (shmaddr == (char *)-1) {
		int err = errno;
		shmctl(shmid, IPC_RMID, NULL);
		throw std::system_error(err, std::generic_category(), "shmat");
	}

	fprintf(stderr, "shm allocated, id=0x%x, addr=%p\n", shmid, shmaddr);

	// Based on linux/tools/testing/selftests/vm/hugepage-shm.c
	fprintf(stderr, "Starting the writes:\n");
	for (size_t i = 0; i < sz; i++) {
		shmaddr[i] = (char)(i);
		if (!(i % (1024 * 1024))) {
			fprintf(stderr, ".");
		}
	}
	fprintf(stderr, "\n");

	fprintf(stderr, "Starting the Check...");
	for (size_t i = 0; i < sz; i++) {
		if (shmaddr[i] != (char) i) {
			fprintf(stderr, "\nIndex %lu mismatched\n", i);
			return 3;
		}
	}
	fprintf(stderr, "Done.\n");

	if (shmdt(shmaddr) < 0) {
		throw std::system_error(errno, std::generic_category(), "shmdt");
	}

	shmctl(shmid, IPC_RMID, NULL);

	return 0;
}
