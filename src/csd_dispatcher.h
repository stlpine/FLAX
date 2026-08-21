/**********************************************************************
 * Copyright (c) 2020-2021
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * *********************************************************************/

#ifndef _NVMEVIRT_CSD_DISPATCHER_H
#define _NVMEVIRT_CSD_DISPATCHER_H

#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>

#include "nvmev.h"
#define ALIGNED_UP(value, align) (((value) + (align)-1) & ~((align)-1))

#define NVMEV_CSD_INFO(ns_name, string, args...)
// #define NVMEV_CSD_INFO(ns_name, string, args...) printk("[CSD_DEBUG][NS:%s] (%s) " string, ns_name, __func__, ##args)

// Async Command — single toggle: 1 = FLAX, 0 = host-managed CSD
#define SUPPORT_ASYNC (1)
#define SUPPORT_ASYNC_MEM_COPY (SUPPORT_ASYNC)
#define SUPPORT_ASYNC_MEM_COPY_DEMAND (SUPPORT_ASYNC)
#define SUPPORT_ASYNC_COMPUTE (SUPPORT_ASYNC_MEM_COPY)
#define SUPPORT_ASYNC_COMMAND (SUPPORT_ASYNC_MEM_COPY || SUPPORT_ASYNC_COMPUTE)

// SLMCPY : memory copy (from TP4131), EXEC : execute program (from TP4091)
// TASK : Unit of processing NVMe CSD Command (SLMCPY or EXEC)
// INTERNAL_IO : SSD I/O for SLMCPY
// If increasing the task_count, you should increaste the task_pool_bits as well
#define MAX_TASK_COUNT (128)
// #define MAX_INTERNAL_IO_COUNT (IO_TOKEN_PER_TASK * IO_REQUEST_SIZE / MIN_IO_REQUEST_SIZE)
#define MAX_INTERNAL_IO_COUNT (256)

/* Generation-based task identifier encoding:
 * Low TASK_POOL_BITS bits = pool index (0..MAX_TASK_COUNT-1)
 * Upper bits = generation counter (wraps every ~33M allocations per slot)
 */
#define TASK_POOL_BITS      7
#define TASK_POOL_MASK      ((1U << TASK_POOL_BITS) - 1)  /* 0x7F */
#define TASK_POOL_INDEX(gen_id)   ((unsigned int)((gen_id) & TASK_POOL_MASK))
#define TASK_ID_NONE        ((uint32_t)-1)
#define TASK_GEN_VALID(gen_id, task_ptr) \
	((gen_id) != TASK_ID_NONE && (task_ptr)->generation_id == (gen_id))

#if (SUPPORT_ASYNC_COMMAND == 1)
#define IO_TOKEN_PER_TASK (1)
#else
#define IO_TOKEN_PER_TASK (16)
#endif
#define MIN_IO_REQUEST_SIZE (16 * 1024)
#define IO_REQUEST_SIZE (NAND_CHANNELS * LUNS_PER_NAND_CH * FLASH_PAGE_SIZE)

// Scheduling Parameters
// CPU core scheduling
#define USE_ONLY_ONE_COMPUTE_CORE (0)
#define USE_IDLE_COMPUTE_CORE (1 || SUPPORT_ASYNC_COMMAND) // support when async
// SLMCPY scheduling
#define CSD_SCHEDULING_TYPE_FIFO (1)
#define CSD_SCHEDULING_TYPE_RR (2)

#if (SUPPORT_ASYNC_COMMAND == 1)
#define CSD_IO_SCHEDULING_TYPE (CSD_SCHEDULING_TYPE_RR)
#else
#define CSD_IO_SCHEDULING_TYPE (CSD_SCHEDULING_TYPE_FIFO)
#endif

struct ccsd_list {
	unsigned int head; /* free io req head index */
	unsigned int tail; /* free io req tail index */

	int running;
};

enum CSD_INTERNAL_IO_REQ_STATUS {
	CSD_INTERNAL_IO_REQ_FREE = 0,
	CSD_INTERNAL_IO_REQ_ALLOC,
	CSD_INTERNAL_IO_REQ_READY,
	CSD_INTERNAL_IO_REQ_WAITING_TIME,
	CSD_INTERNAL_IO_REQ_DONE,
	CSD_INTERNAL_IO_REQ_END,
};

struct csd_internal_io_req {
	unsigned long long nsecs_nand_start;
	unsigned long long nsecs_target;

	uint32_t task_id; /* generation_id of the owning task */
	int io_req_status;

	size_t lba;
	size_t local_offset;  /* Sub-sector byte offset for non-aligned reads */
	size_t buf_addr;
	size_t length;
	size_t stream_offset; /* Logical byte offset of this IO within the input
	                       * stream (head/tail circular rings: buf_addr is
	                       * physical and ambiguous after a wrap) */

	bool last_io;
	bool sequential_last_io;

	bool demand_read;
	unsigned int next, prev;
};

struct csd_io_req_table {
	struct ccsd_list free_list;
	struct ccsd_list *slm_list;
	int slm_turn;

	struct csd_internal_io_req io_req[MAX_INTERNAL_IO_COUNT];
	size_t enqueuing_io_size;
};

enum TASK_STEP { CCSD_TASK_FREE, CCSD_TASK_SCHEDULE, CCSD_TASK_WAIT, CCSD_TASK_END };

struct ccsd_task_info {
	// common info
	atomic_t task_step;
	unsigned int proc_idx;
	bool invalid;
	size_t total_size;
	unsigned int status;

	// I/O parameter
	void *slm_lba_info;
	size_t requested_io_offset;
	size_t done_io_offset;

	int internal_io_count;

	// Compute parameter
	size_t input_buf_addr; // input slm addr
	size_t output_buf_addr; // output slm addr
	unsigned int program_idx;
	unsigned int origin_program_idx; // only meaningful when program_idx == COPY_TO_SLM_PROGRAM_INDEX
	size_t param_size;
	unsigned int params[128];
	size_t result;

	// For streaming data process
	size_t processed_offset;

	// Post process
	unsigned int opcode;

	unsigned int next, prev;
	unsigned long long nsecs_target;

	int host_id;
	int io_quota;

	// for async command
	bool is_first_io;

	uint32_t generation_id; /* globally unique task identifier */
};

struct ccsd_task_table {
	struct ccsd_task_info task[MAX_TASK_COUNT];

	struct ccsd_list free_list;
	struct ccsd_list done_list;
	struct ccsd_list *comp_list;

	int copy_to_slm_list_id;
	int num_slm_resources;
	int num_cpu_resources;
	int compute_turn;

	uint32_t next_generation; /* monotonically increasing generation counter */
};

extern struct ccsd_task_table task_table;

int get_running_task_count(void);
int get_used_io_req_count(void);

#endif
