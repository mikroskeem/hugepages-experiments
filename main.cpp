#include <iterator>
#include <memory>

#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define MM_HUGEPAGES_PATH "/sys/kernel/mm/hugepages"
#define CG_PATH "/sys/fs/cgroup"

#include "cgroups.hpp"
#include "hugepages.hpp"

#define _MAP_HUGE_MASK 0x3f
#define _MAP_HUGE_SHIFT 26
#define HONOR_MLOCK_ULIMIT_DEPRECATION true

#define IS_POW2(n) ((n) > 0 && ((n) & ((n) - 1)) == 0)

int main(int argc, char **argv) {
	// Figure out supported types
	auto supported_hps = hugepage::determine_supported_hps();

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
	if (const auto shift_opt = hugepage::determine_suitable_page_shift(supported_hps, sz)) {
		shift = *shift_opt;
		div = sz / (1 << shift);
		fprintf(stderr, "size=%lu, shift=%lu, div=%lu\n", sz, shift, div);
	} else {
		fprintf(stderr, "size=%lu, does not fit to any supported size!\n", sz);
		return 1;
	}

	const auto available_count = hugepage::get_available_page_count(shift);
	if (available_count && *available_count < div) {
		fprintf(stderr, "Not enough available pages (need=%lu, free=%lu), allocation will fail very likely\n", div, *available_count);
	};

	// Check cgroup limits to avoid nasty SIGBUS
	if (const auto limit_opt = cgroup::check_hugetlb_limit(shift)) {
		auto limit = *limit_opt;
		if (sz > limit) {
			fprintf(stderr, "WARNING: requested size is larger than cgroup hugetlb max limit, adjusting size (%lu > %lu)\n", sz, limit);
			sz = limit;
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
