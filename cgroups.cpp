#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <linux/limits.h>

#include "cgroups.hpp"

std::unique_ptr<size_t> cgroup::check_hugetlb_limit(const unsigned short shift) {
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

	if (hugetlb_max_str == "max") {
		return nullptr;
	}

	return std::make_unique<size_t>(std::stol(hugetlb_max_str));
}
