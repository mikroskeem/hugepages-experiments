#include <algorithm>
#include <cassert>
#include <exception>
#include <fstream>
#include <string>

#include <dirent.h>
#include <libgen.h>
#include <unistd.h>

#include "hugepages.hpp"

#define IS_POW2(n) ((n) > 0 && ((n) & ((n) - 1)) == 0)

inline static unsigned short determine_shift(size_t size) {
        unsigned short shift = 0;
        while (size >>= 1) {
                shift++;
        }
        return shift;
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

        const auto raw_size = filename.substr(prefix.length(), filename.length() - prefix.length() - 2);
	const auto size = std::stol(raw_size) * 1024UL;

	assert(IS_POW2(size));

        return std::make_unique<size_t>(size);
}

std::vector<hugepage::HugepageInfo> hugepage::determine_supported_hps() {
        std::vector<hugepage::HugepageInfo> collected;
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

        return collected;
}

std::unique_ptr<unsigned short> hugepage::determine_suitable_page_shift(const std::vector<hugepage::HugepageInfo> &page_sizes, const size_t total_size) {
        if ((total_size % 2) != 0) {
                return nullptr;
        }

        // Read page sizes in reverse
        for (auto it = page_sizes.rbegin(); it != page_sizes.rend(); ++it) {
                auto elem = *it;
                auto hp_size = elem.first;
                if (total_size >= hp_size && total_size % hp_size == 0) {
                        return std::make_unique<unsigned short>(elem.second);
                }
        }

        return nullptr;
}

std::unique_ptr<size_t> hugepage::get_available_page_count(const size_t shift) {
        auto pagesz_kb = (1 << shift) / 1024;

        char pathbuf[PATH_MAX];
        snprintf(pathbuf, PATH_MAX-1, MM_HUGEPAGES_PATH "/hugepages-%ukB/free_hugepages", pagesz_kb);

        std::ifstream fhp_file(pathbuf);
	if (!fhp_file.good()) {
		return nullptr;
	}

        std::string value;
        fhp_file >> value;

        return std::make_unique<size_t>(std::stol(value));
}
