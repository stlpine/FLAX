KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD     := $(shell pwd)
EXTRAVERSION := $(shell uname -r | grep -oE '\.[0-9]+-[^-]+$$')

ifeq ($(findstring -cslcsd,$(EXTRAVERSION)),)
else
	CONFIG_NVMEVIRT_CSD_eBPF := y
endif

# Select one of the targets to build
#CONFIG_NVMEVIRT_NVM := y
CONFIG_NVMEVIRT_SSD := y
#CONFIG_NVMEVIRT_ZNS := y
#CONFIG_NVMEVIRT_KV := y
CONFIG_NVMEVIRT_CSD := y


obj-m   := nvmev.o
nvmev-objs := main.o pci.o admin.o io.o dma.o
ccflags-y += -Wno-unused-variable -Wno-declaration-after-statement -Wno-frame-larger-than

ccflags-$(CONFIG_NVMEVIRT_NVM) += -DBASE_SSD=INTEL_OPTANE
nvmev-$(CONFIG_NVMEVIRT_NVM) += simple_ftl.o

ccflags-$(CONFIG_NVMEVIRT_SSD) += -DBASE_SSD=SAMSUNG_970PRO
nvmev-$(CONFIG_NVMEVIRT_SSD) += ssd.o conv_ftl.o pqueue.o channel_model.o

ccflags-$(CONFIG_NVMEVIRT_ZNS) += -DBASE_SSD=ZNS_PROTOTYPE
ccflags-$(CONFIG_NVMEVIRT_ZNS) += -Wno-implicit-fallthrough
nvmev-$(CONFIG_NVMEVIRT_ZNS) += ssd.o zns_ftl.o zns_read_write.o zns_mgmt_send.o zns_mgmt_recv.o channel_model.o

ccflags-$(CONFIG_NVMEVIRT_KV) += -DBASE_SSD=KV_PROTOTYPE
nvmev-$(CONFIG_NVMEVIRT_KV) += kv_ftl.o append_only.o bitmap.o

ccflags-$(CONFIG_NVMEVIRT_CSD) += -DCSD_ENABLE=1
nvmev-$(CONFIG_NVMEVIRT_CSD) += csd_dispatcher.o csd_user_func.o csd_ftl.o csd_slm.o buddy.o

# Profiling: keep CSD compute-thread stacks resolvable by a host-side `perf record`
# even if the target kernel's own unwinder config (CONFIG_UNWINDER_ORC) is uncertain.
# Debug info
# (-g) lets `perf report`/`addr2line` resolve nvmev.ko symbols (e.g.
# __rocksdb_mvcc_filter) by name instead of raw addresses. Cheap to always enable;
# comment out if module size/build-time becomes a concern.
ccflags-y += -fno-omit-frame-pointer -g

# Set to 1 (or pass NVMEV_CSD_PROFILE=1 on the make command line) to enable
# src/profile.h's NVMEV_CSD_PROFILE_START/END dmesg tracing around COMPUTE/
# REALCOMPUTE phases (including __rocksdb_mvcc_filter, via
# NVMEV_CSD_PROFILE_REAL_START/END in csd_user_func.c) -- a cheap first profiling
# signal that needs no perf/symbol setup. Leave at 0 for normal benchmark runs
# (the dmesg volume is significant with it on).
NVMEV_CSD_PROFILE ?= 0
ccflags-y += -DNVMEV_CSD_PROFILE_OVERRIDE=$(NVMEV_CSD_PROFILE)

ccflags-$(CONFIG_NVMEVIRT_CSD_eBPF) += -DCSD_eBPF_ENABLE=1
default:
		$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

install:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: clean
clean:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	   rm -f cscope.out tags

.PHONY: cscope
cscope:
		cscope -b -R
		ctags *.[ch]
