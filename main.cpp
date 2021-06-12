#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <dirent.h>
#include <libgen.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/resource.h>
#include <sys/shm.h>

#define MM_HUGEPAGES_PATH "/sys/kernel/mm/hugepages"
#define CG_PATH "/sys/fs/cgroup"

#define _MAP_HUGE_MASK 0x3f
#define _MAP_HUGE_SHIFT 26
#define HONOR_MLOCK_ULIMIT_DEPRECATION true

#define IS_POW2(n) ((n) > 0 && ((n) & ((n) - 1)) == 0)
#define IS_DIV2(n) (((n) % 2) == 0)

using CGHierarchy = std::tuple<unsigned int, std::vector<std::string>, std::string>;

template <typename T>
std::ostream& operator<< (std::ostream& out, const std::vector<T>& v) {
	if (!v.empty()) {
		out << '[';
		std::copy (v.begin(), v.end(), std::ostream_iterator<T>(out, ", "));
		out << "\b\b]";
	}
	return out;
}

static inline void rtrim(std::string &s) {
	s.erase(std::find_if(s.rbegin(), s.rend(), [](auto ch) {
		return !std::isspace(ch);
	}).base(), s.end());
}

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
	std::vector<std::pair<size_t, unsigned short>> collected;
	DIR *dir;
	struct dirent *ent;
	if ((dir = opendir(MM_HUGEPAGES_PATH)) == nullptr) {
		throw std::system_error(errno, std::generic_category(), "opendir " MM_HUGEPAGES_PATH);
	}

	while ((ent = readdir(dir)) != nullptr) {
		char *path = ent->d_name;
		char *filename = basename(path);

		if (const auto hugesize = is_hugepage(filename)) {
			collected.push_back(std::make_pair(*hugesize, determine_shift(*hugesize)));
		}
	}
	closedir(dir);

	// Sort the vector
	std::sort(std::begin(collected), std::end(collected), [](auto a, auto b) {
		return a.first < b.first;
	});

	std::copy(std::begin(collected), std::end(collected), std::back_inserter(supported_hps));
}

static std::unique_ptr<unsigned short> get_page_size(const std::vector<std::pair<size_t, unsigned short>> &page_sizes, const size_t size) {
	if (!IS_DIV2(size)) {
		return nullptr;
	}

	// Read page sizes in reverse
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

static std::unique_ptr<size_t> check_cgroup_limits(const unsigned short shift) {
	// First, figure out this process' cgroup controllers.
	std::ifstream proc_cg_file("/proc/self/cgroup");
	std::string cg_line;
	std::vector<CGHierarchy> collected_cg_hierarchies;
	while (std::getline(proc_cg_file, cg_line)) {
		// Format: hierarchy-ID:controller-list(c0,c1,c2,c3):cg-path
		std::string hier_elem;
		std::vector<std::string> hierarchy;
		std::stringstream cg_line_r(cg_line);
		while (std::getline(cg_line_r, hier_elem, ':')) {
			hierarchy.push_back(hier_elem);
		}

		std::string ctrl_elem;
		std::vector<std::string> controllers;
		std::stringstream hier_line_r(hierarchy[1]);
		while (std::getline(hier_line_r, ctrl_elem, ',')) {
			controllers.push_back(ctrl_elem);
		}

		collected_cg_hierarchies.push_back(std::make_tuple(std::stoi(hierarchy[0]), controllers, hierarchy[2]));
	}

	// Try finding hugetlb controller
	// XXX: supports only cgroups v2 at the moment
	if (collected_cg_hierarchies.size() > 1) {
		throw std::runtime_error("only cgroups v2 is supported");
	}
	auto hugetlb_controller = collected_cg_hierarchies[0];

	// Determine suffix
	std::string szsuffix;
	size_t sz = 0;
	if (shift < 20) {
		szsuffix = "KB";
		sz = (1 << shift) >> 10;
	} else if (shift < 30) {
		szsuffix = "MB";
		sz = (1 << shift) >> 20;
	} else if (shift < 40) {
		szsuffix = "GB";
		sz = (1 << shift) >> 30;
	} else {
		throw std::runtime_error("shift too large");
	}

	// Try to read hugetlb limits
	char pathbuf[PATH_MAX];
	snprintf(pathbuf, PATH_MAX-1, CG_PATH "%s/hugetlb.%lu%s.max", std::get<2>(hugetlb_controller).c_str(), sz, szsuffix.c_str());

	std::ifstream hugetlb_max_file(pathbuf);
	if (!hugetlb_max_file.good()) {
		return nullptr;
	}

	std::string hugetlb_max_str;
	hugetlb_max_file >> hugetlb_max_str;

	return std::make_unique<size_t>(std::stol(hugetlb_max_str));
}

static jmp_buf the_jmpbuf;
static size_t i; // XXX: gross hack

void sigbus(int sig) {
	longjmp(the_jmpbuf, 1);
}

int main(int argc, char **argv) {
	// Set up SIGBUS handler
	signal(SIGBUS, sigbus);

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

	// Check cgroup limits to avoid nasty SIGBUS
	if (const auto limit_opt = check_cgroup_limits(shift)) {
		auto limit = *limit_opt;
		if (sz > limit) {
			fprintf(stderr, "WARNING: requested size is larger than cgroup hugetlb max limit, SIGBUS expected (%lu > %lu)\n", sz, limit);
		} else {
			fprintf(stderr, "NOTE: cgroup hugetlb limit present, max=%lu\n", limit);
		}
	}

	bool memlock_enough = true;
#if HONOR_MLOCK_ULIMIT_DEPRECATION
	memlock_enough = false; // Don't bother
	// TODO: check if we have CAP_IPC_LOCK or user is in group id specified in /proc/sys/vm/hugetlb_shm_group
#else // HONOR_MLOCK_ULIMIT_DEPRECATION
	// Check rlimit
	struct rlimit memlock_cur;
	if (getrlimit(RLIMIT_MEMLOCK, &memlock_cur) < 0) {
		throw std::system_error(errno, std::generic_category(), "getrlimit RLIMIT_MEMLOCK");
	}

	// Adjust limits if needed.
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
#endif // HONOR_MLOCK_ULIMIT_DEPRECATION

	// Allocate using shmget
	int flags = SHM_HUGETLB | IPC_CREAT | SHM_R | SHM_W | ((shift & _MAP_HUGE_MASK) << _MAP_HUGE_SHIFT);
	int shmid = shmget(IPC_PRIVATE, sz, flags);
	if (shmid == -1) {
		int err = errno;
		if (err == EPERM && !memlock_enough) {
			fprintf(stderr, "Caught EPERM while shmget(). Check '/proc/sys/vm/hugetlb_shm_group' or CAP_IPC_LOCK?\n");
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
	size_t maxlen = sz;
	if (setjmp(the_jmpbuf) == 0) {
		for (i = 0; i < maxlen; i++) {
			shmaddr[i] = (char)(i);
			if (!(i % (1024 * 1024))) {
				fprintf(stderr, ".");
			}
		}
		fprintf(stderr, "\n");
	} else {
		fprintf(stderr, "\n*** Caught SIGBUS, tried writing at len=%lu (div=%lu). Adjusting maxlen to it\n", i, i / (1 << shift));
		maxlen = i;
	}

	fprintf(stderr, "Starting the Check...");
	for (i = 0; i < maxlen; i++) {
		if (shmaddr[i] != (char) i) {
			fprintf(stderr, "\nIndex %lu mismatched\n", i);
			shmdt(shmaddr);
			shmctl(shmid, IPC_RMID, NULL);
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
