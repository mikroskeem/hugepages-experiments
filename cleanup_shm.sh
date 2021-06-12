#!/usr/bin/env bash

ipcrm shm "$(ipcs -m | awk '$1 ~ /^0x/ {print $2}')"
