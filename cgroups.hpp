#pragma once
#include <tuple>
#include <vector>

#ifndef CG_PATH
#define CG_PATH "/sys/fs/cgroup"
#endif // CG_PATH

namespace cgroup {
	using CGHierarchy = std::tuple<unsigned int, std::vector<std::string>, std::string>;

	std::vector<CGHierarchy> get_hierarchies();

	std::vector<CGHierarchy> get_hierarchies(const pid_t pid);

	// remaining limit (max - current), max
	std::unique_ptr<std::pair<size_t, size_t>> check_hugetlb_limit(const unsigned short shift);

	// remaining limit (max - current), max
	std::unique_ptr<std::pair<size_t, size_t>> check_hugetlb_limit(const pid_t pid, const unsigned short shift);
}
