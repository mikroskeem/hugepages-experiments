#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <dirent.h>
#include <unistd.h>

#include "cgroups.hpp"

static size_t read_size(std::string path) {
	std::ifstream handle(path);
	if (!handle.good()) {
		return 0;
	}

	std::string str;
	handle >> str;

	if (str == "max") {
		return 0;
	}

	return std::stol(str);
}

std::vector<cgroup::CGHierarchy> cgroup::get_hierarchies() {
	return get_hierarchies(getpid());
}

std::vector<cgroup::CGHierarchy> cgroup::get_hierarchies(const pid_t pid) {
	char pathbuf[PATH_MAX];
	snprintf(pathbuf, PATH_MAX-1, "/proc/%u/cgroup", pid);

	std::ifstream proc_cg_file(pathbuf);
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

	return collected_cg_hierarchies;
}

std::unique_ptr<std::pair<size_t, size_t>> cgroup::check_hugetlb_limit(const unsigned short shift) {
	return cgroup::check_hugetlb_limit(getpid(), shift);
}

std::unique_ptr<std::pair<size_t, size_t>> cgroup::check_hugetlb_limit(const pid_t pid, const unsigned short shift) {
	auto collected_cg_hierarchies = get_hierarchies(pid);

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

	char pathbuf[PATH_MAX];

	// Try reading current max
	snprintf(pathbuf, PATH_MAX-1, CG_PATH "%s/hugetlb.%lu%s.max", std::get<2>(hugetlb_controller).c_str(), sz, szsuffix.c_str());
	size_t hugetlb_max = read_size(pathbuf);
	if (hugetlb_max == 0) {
		return nullptr;
	}

	// Try reading current usage
	snprintf(pathbuf, PATH_MAX-1, CG_PATH "%s/hugetlb.%lu%s.current", std::get<2>(hugetlb_controller).c_str(), sz, szsuffix.c_str());
	size_t hugetlb_current = read_size(pathbuf);

	size_t hugetlb_limit = hugetlb_max - hugetlb_current;
	std::pair<size_t, size_t> p(hugetlb_limit, hugetlb_max);

	return std::make_unique<std::pair<size_t, size_t>>(p);
}
