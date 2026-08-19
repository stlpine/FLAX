#!/bin/bash
#
# Bare-metal values for this host (2x Intel Xeon Silver 4316, 40 logical CPUs
# / 2 NUMA nodes, 1 thread/core -- see init_nvmev.sh for the full topology
# note).
# node0 (even 0-38) = app cores (mysqld+OLTP/OLAP): ondemand governor, no
# throttling. node1 (odd 1-39) = FLAX: cpus=1,3,5,7 + slm_cpus=9,11,13,15
# (dispatcher/NAND-SLM transfer, run at full "performance" speed) and
# csd_cpus=17..39 (the emulated ARM compute cores -- these are the ones
# actually throttled to 1GHz to approximate a wimpy ARM core, per the paper).
echo "1"
for c in 0 2 4 6 8 10 12 14 16 18 20 22 24 26 28 30 32 34 36 38
do
    sudo cpufreq-set -g ondemand -c $c
done

echo "2"
for c in 1 3 5 7 9 11 13 15
do
    sudo cpufreq-set -g performance -c $c
done

# CSD Cores
comp_clock=1000MHz
for c in 17 19 21 23 25 27 29 31 33 35 37 39
do
    echo "Set cpu $c"
    sudo cpufreq-set -g userspace -c $c
    sudo cpufreq-set -f $comp_clock -c $c
done
echo ""

for c in 1 3 5 7 9 11 13 15
do
sudo grep . /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq
done

for c in 17 19 21 23 25 27 29 31 33 35 37 39
do
sudo grep . /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq
done


sync
sync
sync

sudo sh -c 'echo 1 > /proc/sys/vm/drop_caches '
sudo sh -c 'echo 2 > /proc/sys/vm/drop_caches '
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches '

sync
sync
sync
