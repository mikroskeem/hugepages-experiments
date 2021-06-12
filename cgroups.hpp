#pragma once
#include <tuple>
#include <vector>

#ifndef CG_PATH
#define CG_PATH "/sys/fs/cgroup"
#endif // CG_PATH

namespace cgroup {
	using CGHierarchy = std::tuple<unsigned int, std::vector<std::string>, std::string>;

	std::unique_ptr<size_t> check_hugetlb_limit(const unsigned short shift);
}
