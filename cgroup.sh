#!/usr/bin/env bash
set -euo pipefail
set -x

cg_name="hp-experim"

if ! [ "$(id -u)" = 0 ]; then
	exec sudo -E env ORIG_UID="$(id -u)" ORIG_GID="$(id -g)" "${0}" "${@}"
fi

cgpath="$(awk '$1 == "cgroup2" { print $2 }' /proc/mounts)"
if [ -z "${cgpath}" ]; then
	echo ">>> Could not find cgroup2 mount point!"
	exit 1
fi

enable_controller () {
	local cg="${1}"
	local what="${2}"
	if ! grep -q "${what}" "${cg}"/cgroup.subtree_control; then
		echo "+${what}" > "${cg}"/cgroup.subtree_control
	fi
}

# Ensure that hugetlb controller is enabled
enable_controller "${cgpath}" "hugetlb"

# Create our test cg
if [ -d "${cgpath}/${cg_name}" ]; then
	rmdir "${cgpath}/${cg_name}"
fi

mkdir "${cgpath}/${cg_name}"

# Add itself to the cg
echo "$$" > "${cgpath}/${cg_name}/cgroup.procs"

# Ensure that hugetlb is enabled in child as well
enable_controller "${cgpath}/${cg_name}" "hugetlb" || true

# Set up hugetlb limits
# In this case, do 1GB
echo $(( 1 * 1073741824 )) > "${cgpath}/${cg_name}/hugetlb.1GB.max"

if [ -n "${*:-}" ]; then
	exec sudo -u "#${ORIG_UID}" -g "#${ORIG_GID}" -- "${@}"
else
	echo ">>> Starting shell in the cgroup"
	exec sudo -u "#${ORIG_UID}" -g "#${ORIG_GID}" bash -i
fi
