#pragma once
#include <memory>
#include <tuple>
#include <vector>

#ifndef MM_HUGEPAGES_PATH
#define MM_HUGEPAGES_PATH "/sys/kernel/mm/hugepages"
#endif // MM_HUGEPAGES_PATH

namespace hugepage {
	using HugepageInfo = std::pair<size_t, unsigned short>;

	std::vector<HugepageInfo> determine_supported_hps();

	std::unique_ptr<unsigned short> determine_suitable_page_shift(const std::vector<HugepageInfo> &page_sizes, const size_t total_size);

	std::unique_ptr<size_t> get_available_page_count(const size_t shift);
}
