// Overridable at build time: `make NVMEV_CSD_PROFILE=1` (see src/Makefile) turns
// this into dmesg-based COMPUTE/REALCOMPUTE tracing (incl. __rocksdb_mvcc_filter,
// via NVMEV_CSD_PROFILE_REAL_START/END) with no perf/symbol setup required.
#ifndef NVMEV_CSD_PROFILE_OVERRIDE
#define NVMEV_CSD_PROFILE_OVERRIDE (0)
#endif
#define NVMEV_CSD_PROFILE		(NVMEV_CSD_PROFILE_OVERRIDE)

#if (NVMEV_CSD_PROFILE == 0)
#define NVMEV_CSD_PROFILE_START(core_name, core_num, task_id, subtask_id)
#define NVMEV_CSD_PROFILE_START_WITH_TIME(core_name, core_num, task_id, subtask_id, cpu_time)
#define NVMEV_CSD_PROFILE_END(core_name, core_num, task_id, subtask_id)
#define NVMEV_CSD_PROFILE_WITH_TIME(core_name, core_num, task_id, subtask_id, start_time, end_time)
#else
#define NVMEV_CSD_PROFILE_START(core_name, core_num, task_id, subtask_id) \
 	printk("[CCSD_PROFILE] %s_%d_TASK_%d_%d_START %lld", core_name, core_num, task_id, subtask_id, __get_wallclock())
#define NVMEV_CSD_PROFILE_START_WITH_TIME(core_name, core_num, task_id, subtask_id, cpu_time) \
 	printk("[CCSD_PROFILE] %s_%d_TASK_%d_%d_START %lld", core_name, core_num, task_id, subtask_id, cpu_time)
#define NVMEV_CSD_PROFILE_END(core_name, core_num, task_id, subtask_id) \
 	printk("[CCSD_PROFILE] %s_%d_TASK_%d_%d_END %lld", core_name, core_num, task_id, subtask_id, __get_wallclock())

#define NVMEV_CSD_PROFILE_WITH_TIME(core_name, core_num, task_id, subtask_id, start_time, end_time)             \
 	printk("[CCSD_PROFILE] %s_%d_TASK_%d_%d_START %lld", core_name, core_num, task_id, subtask_id, start_time); \
 	printk("[CCSD_PROFILE] %s_%d_TASK_%d_%d_END %lld", core_name, core_num, task_id, subtask_id, end_time);
#endif
