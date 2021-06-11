#!/usr/bin/env bash

grep -R "" /sys/kernel/mm/hugepages/ /proc/sys/vm/*huge*
