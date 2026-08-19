#!/bin/bash

echo "Setup"
sudo umount /mnt/nvme
sudo rmmod nvmev
sudo nvme list
make clean || exit
make -j 8 || exit

# Bare-metal values for this host: 2x Intel Xeon Silver 4316, 40 logical
# CPUs / 2 NUMA nodes, 1 thread/core (no hyperthreading -- gotcha #6 re: not
# splitting a physical core's SMT siblings across throttle groups does not
# apply here, every CPU number below is its own distinct physical core).
#
# node0 = even CPUs 0,2,...,38 (~251GiB) -- reserved for mysqld+OLTP/OLAP.
# node1 = odd CPUs 1,3,...,39 (~255GiB total; ~228GiB of it already reserved
# via memmap=228G$280G in GRUB_CMDLINE_LINUX_DEFAULT, confirmed via
# /proc/cmdline post-boot) -- used here for FLAX.
#
# CPU split follows the same 4/4/12 (cpus/slm_cpus/csd_cpus) ratio already
# validated on a previous, older deployment host, scaled to node1's full 20
# physical cores here (that older host had only 10 physical cores available
# per node due to its older CPU generation).
echo "Load NVMeVirt kernel module..."
sudo insmod nvmev.ko \
	memmap_start=280 memmap_size=233472 slm_size=4096 \
	cpus=1,3,5,7 \
	slm_cpus=9,11,13,15 \
	csd_cpus=17,19,21,23,25,27,29,31,33,35,37,39


echo "Set CPU Frequency"
source perf.sh

sleep 3
./mount.sh

cd lib
./build.sh
