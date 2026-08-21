#define CSDVIRT_USER_PROGRAM
#include <linux/crypto.h>

#include "nvmev.h"
#include "csd_slm.h"
#include "csd_user_func.h"
#include "user_func_rocksdb.h"
#include "user_func_xxh3.h"
#include "user_func_bloom.h"
#include "user_func_rocksdb_properties.h"
#include "csd_dispatcher.h"

extern struct nvmev_dev *vdev;

/* Pre-allocated buffers for compaction to avoid repeated kmalloc/vmalloc */
#define INDEX_BLOCK_BUF_SIZE (1024 * 1024)      // 1 MB
#define INDEX_RESTART_BUF_SIZE (1024 * 1024)    // 1 MB
#define HASH_ENTRY_BUF_SIZE (5 * 1024 * 1024)   // 5 MB
#define TEMP_BLOCK_BUF_SIZE (1024 * 1024)       // 1 MB (for filter, properties, metaindex)

static char *g_index_block_ptr;
static char *g_index_restart_ptr;
static uint64_t *g_hash_entry_ptr;
static char *g_temp_block_ptr;

int csd_user_func_init(void)
{
	g_index_block_ptr = kmalloc_node(INDEX_BLOCK_BUF_SIZE, GFP_KERNEL, 1);
	if (!g_index_block_ptr)
		goto fail_index_block;

	g_index_restart_ptr = kmalloc_node(INDEX_RESTART_BUF_SIZE, GFP_KERNEL, 1);
	if (!g_index_restart_ptr)
		goto fail_index_restart;

	g_hash_entry_ptr = vmalloc_node(HASH_ENTRY_BUF_SIZE, 1);
	if (!g_hash_entry_ptr)
		goto fail_hash_entry;

	g_temp_block_ptr = kmalloc(TEMP_BLOCK_BUF_SIZE, GFP_KERNEL);
	if (!g_temp_block_ptr)
		goto fail_temp_block;

	return 0;

fail_temp_block:
	vfree(g_hash_entry_ptr);
fail_hash_entry:
	kfree(g_index_restart_ptr);
fail_index_restart:
	kfree(g_index_block_ptr);
fail_index_block:
	return -ENOMEM;
}

void csd_user_func_exit(void)
{
	kfree(g_temp_block_ptr);
	vfree(g_hash_entry_ptr);
	kfree(g_index_restart_ptr);
	kfree(g_index_block_ptr);
}

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_dispatcher);
}

#if ((CSD_ENABLE))
#define NVMEV_CSD_PROFILE_REAL_START(core_num, task_id) NVMEV_CSD_PROFILE_START("REALCOMPUTE", core_num, task_id, 0)
#define NVMEV_CSD_PROFILE_REAL_END(core_num, task_id) NVMEV_CSD_PROFILE_END("REALCOMPUTE", core_num, task_id, 0)
#else
#define NVMEV_CSD_PROFILE_REAL_START(core_num, task_id)
#define NVMEV_CSD_PROFILE_REAL_END(core_num, task_id)
#endif

size_t __csdvirt_run_program(int program_idx, void *buf_in, void *buf_out, size_t size, void *params)
{
	size_t ret = 0;
	if (program_idx == ROCKSDB_COMPACTION_PROGRAM_INDEX) {
		ret = __rocksdb_compaction((char *)buf_in, (char *)buf_out, size, params);
	} else if (program_idx == ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX) {
		ret = __magic_rocksdb_compaction(params);
	} else if (program_idx == ROCKSDB_MAGIC_CRC_PROGRAM_INDEX) {
		ret = __magic_rocksdb_crc_calculation(params);
	} else if (program_idx == ROCKSDB_MAGIC_READ_PROGRAM_INDEX) {
		ret = __magic_rocksdb_read(params);
	} else if (program_idx == ROCKSDB_CRC_PROGRAM_INDEX) {
		ret = __rocksdb_crc_calculation((char *) buf_in, (char *) buf_out, size, params);
	} else if (program_idx == ROCKSDB_READ_PROGRAM_INDEX) {
		ret = __rocksdb_read((char *) buf_in, (char *) buf_out, size, params);
	} else if (program_idx == ROCKSDB_MVCC_FILTER_PROGRAM_INDEX) {
		ret = __rocksdb_mvcc_filter((char *) buf_in, (char *) buf_out, size, params);
	} else {
		printk("INVALID PROGRAM_INDEX %d\n", program_idx);
	}

	return ret;
}

/* CONSUMER barrier (non-compound + compound-read type 1): a compute program calls this
 * before reading [ptr,size). Polls check_slm_data_ready; on a miss it kicks a
 * demand load (slm_request_demand_read) and spins until the data is ready (or the
 * producing stage has finalized, meaning there is no more data to wait for). */
int check_data_using_ptr(size_t ptr, size_t size, int pid, int host_id)
{
	if (check_slm_data_ready(ptr, size, true) == false) {
#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 1)
		slm_request_demand_read(ptr, size);
#endif
		// return NULL;

		/* In case of sequence of execution, check if prior execution is done */
		if (check_slm_output_data_finalized(ptr, size) == true) {
			return 0;
		}
		NVMEV_CSD_PROFILE_REAL_END(pid, host_id);
		while (check_slm_data_ready(ptr, size, false) == false) {
			if (check_slm_output_data_finalized(ptr, size) == true) {
				return 0;
			}
			cond_resched();
		}
		NVMEV_CSD_PROFILE_REAL_START(pid, host_id);
	}
	return 1;
}

/* CONSUMER barrier for compound-read type 0 (the _info variant). Same poll/demand/
 * spin loop as check_data_using_ptr, but routed through check_slm_data_ready_info
 * + slm_request_demand_read_info so the demand goes through the COMPACT extent
 * allocator and the region flips use_head_tail -> extent_mapped on the first jump. */
int check_data_using_ptr_info(size_t ptr, size_t size, int pid, int host_id, struct slm_lba_info *info)
{
	if (check_slm_data_ready_info(ptr, size, true, info) == false) {
		slm_request_demand_read_info(ptr, size, info);
		if (check_slm_output_data_finalized_info(ptr, size, info) == true) {
			return 0;
		}
		NVMEV_CSD_PROFILE_REAL_END(pid, host_id);
		{
		unsigned long start_jiffies = jiffies;
		bool timeout_warned = false;
		while (check_slm_data_ready_info(ptr, size, false, info) == false) {
			if (check_slm_output_data_finalized_info(ptr, size, info) == true) {
				return 0;
			}
			if (!timeout_warned && time_after(jiffies, start_jiffies + 15 * HZ)) {
				uint64_t start_pg = get_slm_offset(ptr) / SLM_PAGE_SIZE;
				uint64_t end_pg = get_slm_offset(ptr + size - 1) / SLM_PAGE_SIZE;

				NVMEV_ERROR("CPU%d check_data_using_ptr_info: spinning >15s ptr=0x%lx size=%lu pages=[%llu-%llu]\n",
					smp_processor_id(), (unsigned long)ptr, (unsigned long)size, start_pg, end_pg);

				NVMEV_ERROR("CPU%d   ht_head=%lu ht_tail=%lu extent_count=%d\n",
					smp_processor_id(), info->ht_head, info->ht_tail, info->extent_count);

				NVMEV_ERROR("CPU%d   slm_lba_info: task_id=%u request_done=%d load_complete=%d"
					" demand_read_offset=%llu demand_read_size=%lu"
					" next_addr=%llu is_output=%d is_circular=%d extent_mapped=%d\n",
					smp_processor_id(),
					info->task_id,
					info->request_done,
					info->load_complete,
					(unsigned long long)info->demand_read_offset,
					(unsigned long)info->demand_read_size,
					(unsigned long long)info->next_addr,
					info->is_output,
					info->is_circular,
					info->extent_mapped);

				timeout_warned = true;
			}
			smp_rmb();
			cond_resched();
		}
		}
		NVMEV_CSD_PROFILE_REAL_START(pid, host_id);
	}
	return 1;
}

void set_data_from_ptr(size_t dst, size_t src, size_t size)
{
	// printk("src: %lu, dst: %lu, size: %lu\n", src, dst, size);
	memcpy((void *)dst, (void *)src, size);

	notify_if_slm_data_ready(dst, size);
}

/* Initialize circular buffer context */
static inline void circular_buf_init(struct circular_buf_ctx *ctx, size_t base_addr,
				     size_t logical_total_size, struct slm_lba_info *lba_info,
				     const char *debug_tag)
{
	ctx->base_addr = base_addr;
	ctx->logical_offset = 0;
	ctx->logical_total_size = logical_total_size;
	ctx->prev_logical_offset = 0;
	ctx->lba_info = lba_info;
	ctx->debug_tag = debug_tag;
	CSD_DEBUG_MAGIC_OUTPUT("Circular buffer init: base_addr=%p, logical_total_size=%lu, lba_info=%p, debug_tag=%s\n",
	       (void *)base_addr, logical_total_size, lba_info, debug_tag);
}

/* Check if all data has been loaded (IO complete) */
static inline bool circular_buf_load_complete(struct circular_buf_ctx *ctx)
{
	return ctx->lba_info->load_complete;
}

/* Convert logical offset to physical pointer within circular buffer */
static inline char *circular_buf_ptr(struct circular_buf_ctx *ctx, size_t logical_offset)
{
	size_t physical_offset = logical_offset & MAGIC_BUFFER_MASK;
	return (char *)(ctx->base_addr + physical_offset);
}

/* Get current pointer based on ctx->logical_offset */
static inline char *circular_buf_current_ptr(struct circular_buf_ctx *ctx)
{
	return circular_buf_ptr(ctx, ctx->logical_offset);
}

/* Check if we've reached the end of logical data */
static inline bool circular_buf_at_end(struct circular_buf_ctx *ctx)
{
	return ctx->logical_offset >= ctx->lba_info->logical_total_size;
}

/* Get remaining logical bytes */
static inline size_t circular_buf_remaining(struct circular_buf_ctx *ctx)
{
	if (ctx->logical_offset >= ctx->logical_total_size)
		return 0;
	return ctx->logical_total_size - ctx->logical_offset;
}

/* CONSUMER barrier for a compound circular INPUT ring (the ring counterpart of
 * check_data_using_ptr). Compares the LOGICAL offset against ht_head and spins
 * until ready or the load is complete; rings never flip to demand/extent. */
static __always_inline int circular_buf_check_data_at(struct circular_buf_ctx *ctx,
						      size_t logical_offset, size_t size,
						      int pid, int host_id)
{
	/* If all data has been loaded, don't wait - data is already there */
	size_t physical_offset = logical_offset & MAGIC_BUFFER_MASK;
	char *ptr = (char *)(ctx->base_addr + physical_offset);

#if (USE_HEAD_TAIL_DEP)
	if (ctx->lba_info->use_head_tail) {
		/* Head/tail ring: readiness is a LOGICAL compare against the produced
		 * frontier (physical ring addresses are ambiguous after a wrap).
		 * Page-granular; a size==SLM_PAGE_SIZE access only checks its start
		 * page. */
		size_t last_page = (size == SLM_PAGE_SIZE)
					   ? (logical_offset / SLM_PAGE_SIZE)
					   : ((logical_offset + size - 1) / SLM_PAGE_SIZE);
		smp_rmb();
		if (last_page < DIVIDE_UP(ctx->lba_info->ht_head, SLM_PAGE_SIZE))
			return 1;
#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 1)
		/* stream_access handshake: lets the loader's look-ahead quota engage
		 * (safe: the is_circular branch there never flips to demand mode). */
		slm_request_demand_read((size_t)ptr, size);
#endif
		do {
			if (circular_buf_load_complete(ctx)) {
				return 1;
			}
			cond_resched();
			smp_rmb();
		} while (last_page >= DIVIDE_UP(ctx->lba_info->ht_head, SLM_PAGE_SIZE));
		return 1;
	}
#endif

	if (check_slm_data_ready((size_t)ptr, size, true) == false) {
#if (SUPPORT_ASYNC_MEM_COPY_DEMAND == 1)
		slm_request_demand_read((size_t)ptr, size);
#endif
		CSD_DEBUG_MAGIC_INPUT_CTX(ctx->debug_tag, "Waiting for input data: logical_offset=%lu, slm_page=%lu, size=%lu\n",
			  logical_offset, get_slm_offset((size_t)ptr) >> SLM_PAGE_SHIFT, size);
		while (check_slm_data_ready((size_t)ptr, size, false) == false) {
			if (circular_buf_load_complete(ctx)) {
				return 1;
			}
			cond_resched();
		}
	}

	return 1;
}


static __always_inline int circular_buf_check_data(struct circular_buf_ctx *ctx, size_t size,
						   int pid, int host_id)
{
	return circular_buf_check_data_at(ctx, ctx->logical_offset, size, pid, host_id);
}

/* Notify compute progress for circular buffer.
 * Uses notify_compute_ready_circular which also clears is_ready,
 * so the next wrap-around will wait for new data to be loaded.
 * Also updates compute_logical_offset in slm_lba_info for IO throttling.
 * Handles cases where multiple pages are processed at once (curr_page - prev_page > 1). */
static inline void circular_buf_notify_progress(struct circular_buf_ctx *ctx)
{
	size_t prev_page;
	size_t curr_page;
#if (_MAGIC_DEBUG_INPUT_ == 1)
	size_t base_slm_page = get_slm_offset(ctx->base_addr) >> SLM_PAGE_SHIFT;
#endif

	/* Throttle to HT_NOTIFY_GRAIN (monotonic logical offsets, so the compare
	 * is wrap-unambiguous): the tail store below invalidates the cacheline the
	 * upstream producer reads per entry, so keep it off the per-page hot path.
	 * Clamped to >= SLM_PAGE_SIZE so the masked page walk below always moves. */
	{
		size_t grain = (HT_NOTIFY_GRAIN > SLM_PAGE_SIZE) ? HT_NOTIFY_GRAIN : SLM_PAGE_SIZE;
		if (likely(ctx->prev_logical_offset / grain == ctx->logical_offset / grain))
			return; /* No grain change */
	}

	prev_page = (ctx->prev_logical_offset & MAGIC_BUFFER_MASK) >> SLM_PAGE_SHIFT;
	curr_page = (ctx->logical_offset & MAGIC_BUFFER_MASK) >> SLM_PAGE_SHIFT;

	CSD_DEBUG_MAGIC_INPUT_CTX(ctx->debug_tag, "Notify progress: prev_log=%lu, curr_log=%lu, prev_slm_page=%lu, curr_slm_page=%lu\n",
		  ctx->prev_logical_offset, ctx->logical_offset, base_slm_page + prev_page, base_slm_page + curr_page);

	if (curr_page > prev_page) {
		/* No wrap-around: notify all pages from prev_page to curr_page-1 */
		for (size_t page = prev_page; page < curr_page; page++) {
			char *page_ptr = (char *)(ctx->base_addr + (page << SLM_PAGE_SHIFT));
			notify_compute_ready_circular((size_t)page_ptr);
		}
	} else {
		/* Wrap-around: curr_page < prev_page
		 * First, notify all pages from prev_page to end of buffer */
		for (size_t page = prev_page; page < MAGIC_BUFFER_PAGES; page++) {
			char *page_ptr = (char *)(ctx->base_addr + (page << SLM_PAGE_SHIFT));
			notify_compute_ready_circular((size_t)page_ptr);
		}
		/* Mark ghost page as compute_done */
		char *ghost_ptr = (char *)(ctx->base_addr + MAGIC_BUFFER_SIZE);
		notify_compute_ready_circular((size_t)ghost_ptr);
		CSD_DEBUG_MAGIC_INPUT_CTX(ctx->debug_tag, "Wrap-around: marking ghost page as compute_done, ghost_slm_page=%lu\n",
			  get_slm_offset((size_t)ghost_ptr) >> SLM_PAGE_SHIFT);
		/* Then notify all pages from 0 to curr_page-1 */
		for (size_t page = 0; page < curr_page; page++) {
			char *page_ptr = (char *)(ctx->base_addr + (page << SLM_PAGE_SHIFT));
			notify_compute_ready_circular((size_t)page_ptr);
		}
	}

	ctx->prev_logical_offset = ctx->logical_offset;

	/* Update compute_logical_offset for IO throttling */
	update_circular_compute_offset(ctx->base_addr, ctx->logical_offset);
}

static inline void circular_output_buf_init(struct circular_output_buf_ctx *ctx,
					    size_t base_addr,
					    struct slm_lba_info *lba_info,
					    const char *debug_tag)
{
	ctx->base_addr = base_addr;
	ctx->base_page = get_slm_offset(base_addr) >> SLM_PAGE_SHIFT;
	ctx->logical_write_offset = 0;
	ctx->prev_logical_offset = 0;
	ctx->lba_info = lba_info;
	ctx->debug_tag = debug_tag;
	CSD_DEBUG_MAGIC_INPUT("Circular output buffer init: base_addr=%p, lba_info=%p, debug_tag=%s\n",
	       (void *)base_addr, lba_info, debug_tag);
}

/*
 * circular_output_buf_ptr - Convert logical offset to physical pointer
 * @ctx: Output buffer context
 * @logical_offset: Logical offset into the output stream
 *
 * Returns: Physical pointer within the circular buffer
 *
 * Maps logical offset to physical address using modulo arithmetic.
 * The caller must ensure the data at this offset hasn't been overwritten.
 */
static inline char *circular_output_buf_ptr(struct circular_output_buf_ctx *ctx,
					    size_t logical_offset)
{
	size_t physical_offset = logical_offset & MAGIC_BUFFER_MASK;
	return (char *)(ctx->base_addr + physical_offset);
}

static __always_inline void circular_output_wait_for_space(struct circular_output_buf_ctx *ctx,
					   size_t size)
{
	/* Head/tail ring: the write [W, W+size) may proceed once the consumer's
	 * frontier is within one buffer of the END of the write's last page.
	 * First wrap passes trivially (need <= MAGIC_BUFFER_SIZE). */
	size_t need = ALIGNED_DOWN(ctx->logical_write_offset + size - 1, SLM_PAGE_SIZE)
		      + SLM_PAGE_SIZE;
	while (need > ctx->lba_info->ht_tail + MAGIC_BUFFER_SIZE) {
		smp_rmb();
		cond_resched();
	}
}

static inline void circular_output_set_data(struct circular_output_buf_ctx *ctx,
						     const void *src, size_t size)
{
	/* Wait if we would overwrite unconsumed data */
	circular_output_wait_for_space(ctx, size);

	size_t physical_offset = ctx->logical_write_offset & MAGIC_BUFFER_MASK;
	size_t bytes_to_end = MAGIC_BUFFER_SIZE - physical_offset;
	char *dst = (char *)(ctx->base_addr + physical_offset);

	if (likely(size <= bytes_to_end)) {
		/* Case 1: Data fits without wrap-around (common case) */
		memcpy(dst, src, size);

		/* Mirror to ghost page if writing to first GHOST_PAGE_SIZE bytes */
		if (unlikely(physical_offset < GHOST_PAGE_SIZE)) {
			size_t ghost_copy_size = min(size, GHOST_PAGE_SIZE - physical_offset);
			char *ghost_dst = (char *)(ctx->base_addr + MAGIC_BUFFER_SIZE + physical_offset);
			memcpy(ghost_dst, src, ghost_copy_size);
		}

		/* Update logical offset */
		ctx->logical_write_offset += size;

		/* Publish the monotonic logical head. */
		notify_circular_output_produced(ctx->lba_info, ctx->logical_write_offset);
	} else {
		/* Case 2: Data wraps around - split into two copies */
		/* First part: from current position to end of buffer */
		memcpy(dst, src, bytes_to_end);

		/* Second part: from beginning of buffer */
		size_t remaining = size - bytes_to_end;
		char *wrap_dst = (char *)(ctx->base_addr);
		memcpy(wrap_dst, (const char *)src + bytes_to_end, remaining);

		/* Mirror wrapped portion to ghost page */
		if (remaining <= GHOST_PAGE_SIZE) {
			char *ghost_dst = (char *)(ctx->base_addr + MAGIC_BUFFER_SIZE);
			memcpy(ghost_dst, (const char *)src + bytes_to_end, remaining);
		} else {
			/* Wrapped data exceeds ghost page - copy only ghost page size */
			char *ghost_dst = (char *)(ctx->base_addr + MAGIC_BUFFER_SIZE);
			memcpy(ghost_dst, (const char *)src + bytes_to_end, GHOST_PAGE_SIZE);
		}

		/* Update logical offset */
		ctx->logical_write_offset += size;

		/* Monotonic logical head: the wrap-split collapses to one publish. */
		notify_circular_output_produced(ctx->lba_info, ctx->logical_write_offset);
	}
}

static __always_inline void circular_output_set_data_from_circular(
	struct circular_output_buf_ctx *dst_ctx,
	struct circular_buf_ctx *src_ctx,
	size_t src_logical_offset,
	size_t size)
{
	circular_output_wait_for_space(dst_ctx, size);

	size_t src_physical = src_logical_offset & MAGIC_BUFFER_MASK;
	size_t dst_physical = dst_ctx->logical_write_offset & MAGIC_BUFFER_MASK;
	size_t src_to_end = MAGIC_BUFFER_SIZE - src_physical;
	size_t dst_to_end = MAGIC_BUFFER_SIZE - dst_physical;

	char *src = (char *)(src_ctx->base_addr + src_physical);
	char *dst = (char *)(dst_ctx->base_addr + dst_physical);

	bool src_wraps = (size > src_to_end);
	bool dst_wraps = (size > dst_to_end);

	/* The src-wrap path below reads size bytes contiguously, valid only while
	 * the overhang past the ring end stays within the ghost mirror */
	if (src_wraps)
		WARN_GHOST_OVERRUN(size - src_to_end);

	if (likely(!src_wraps && !dst_wraps)) {
		/* Neither wraps - simple copy */
		memcpy(dst, src, size);
		/* Ghost page if dst in first GHOST_PAGE_SIZE */
		if (unlikely(dst_physical < GHOST_PAGE_SIZE)) {
			size_t ghost_size = min(size, GHOST_PAGE_SIZE - dst_physical);
			memcpy((char *)(dst_ctx->base_addr + MAGIC_BUFFER_SIZE + dst_physical), src, ghost_size);
		}
	} else {
		/* At least one wraps - use ghost page for contiguous source read,
		 * then handle destination wrap */
		char *src_contiguous = src;  /* Ghost page makes src contiguous */
		if (!dst_wraps) {
			memcpy(dst, src_contiguous, size);
			if (unlikely(dst_physical < GHOST_PAGE_SIZE)) {
				size_t ghost_size = min(size, GHOST_PAGE_SIZE - dst_physical);
				memcpy((char *)(dst_ctx->base_addr + MAGIC_BUFFER_SIZE + dst_physical), src_contiguous, ghost_size);
			}
		} else {
			/* Destination wraps */
			memcpy(dst, src_contiguous, dst_to_end);
			memcpy((char *)dst_ctx->base_addr, src_contiguous + dst_to_end, size - dst_to_end);
			/* Ghost page for wrapped portion */
			size_t ghost_size = min(size - dst_to_end, (size_t)GHOST_PAGE_SIZE);
			memcpy((char *)(dst_ctx->base_addr + MAGIC_BUFFER_SIZE), src_contiguous + dst_to_end, ghost_size);
		}
	}

	dst_ctx->logical_write_offset += size;

	/* Publish the monotonic logical head. */
	notify_circular_output_produced(dst_ctx->lba_info, dst_ctx->logical_write_offset);
}

/*
 * circular_output_notify_range - Notify a range as ready without writing data
 * @ctx: Output buffer context
 * @size: Number of bytes to notify as ready

 * No actual data is written - this just advances the produced head for the range.
 * Also waits for host to consume data if we would overwrite unconsumed pages.
 */
static void circular_output_notify_range(struct circular_output_buf_ctx *ctx,
					 size_t size)
{
	if (size == 0)
		return;

	/* Wait if we would overwrite unconsumed data */
	circular_output_wait_for_space(ctx, size);

	ctx->logical_write_offset += size;
	notify_circular_output_produced(ctx->lba_info, ctx->logical_write_offset);
}

/*
 * circular_output_finalize - Mark output as complete
 * @ctx: Output buffer context
 *
 * Called when compaction is complete. Sets output_logical_total_size so the
 * host knows the final size of the output and can read all remaining data.
 * Also marks the final partial page as ready so the host can read it.
 */
static void circular_output_finalize(struct circular_output_buf_ctx *ctx)
{
	/* Mark the final page as ready.
	 * notify_if_slm_data_ready only marks full pages, so we need to
	 * explicitly mark the final partial page so the host can read it.
	 *
	 * logical_write_offset points one past the last written byte.
	 * We need to mark the page containing the last written byte.
	 * If logical_write_offset is page-aligned, the last byte is in the previous page.
	 * If not page-aligned, the last byte is in the current partial page. */
#if (USE_HEAD_TAIL_DEP)
	if (ctx->lba_info->use_head_tail) {
		/* Byte-exact head publishes the final partial page (page-granular
		 * readiness rounds up via DIVIDE_UP; the host byte-compare is bounded
		 * by the end clamp). Producer is done: later notifies are ignored. */
		ctx->lba_info->ht_head = ctx->logical_write_offset;
		ctx->lba_info->ht_producer_done = true;
	} else
#endif
	if (ctx->logical_write_offset > 0) {
		size_t last_byte_logical = ctx->logical_write_offset - 1;
		size_t last_byte_physical = last_byte_logical & MAGIC_BUFFER_MASK;
		size_t final_page_addr = ctx->base_addr + ((last_byte_physical >> SLM_PAGE_SHIFT) << SLM_PAGE_SHIFT);
		/* Mark the final page as ready */
		notify_slm_data_ready(final_page_addr, SLM_PAGE_SIZE);
		CSD_DEBUG_MAGIC_OUTPUT_CTX(ctx->debug_tag, "Output finalized: marking final slm_page=%lu\n",
			  get_slm_offset(final_page_addr) >> SLM_PAGE_SHIFT);
	}

	ctx->lba_info->output_logical_total_size = ctx->logical_write_offset;
	ctx->lba_info->load_complete = true;  /* For CRC input chaining */
	ctx->lba_info->logical_total_size = ctx->logical_write_offset; /* For CRC input chaining */
	smp_wmb();

	CSD_DEBUG_MAGIC_OUTPUT_CTX(ctx->debug_tag, "Output finalized: total_size=%lu\n", ctx->logical_write_offset);
}

size_t __rocksdb_finalized_sstable(void *buf_file_out, size_t output_offset,
								   char *index_block_ptr, char *index_block_append_ptr,
								   char *index_restart_ptr,
								   char *index_restart_append_ptr,
								   uint32_t restart_entry_count,
									int bits_per_key, bool bloom_flag,
									uint64_t *hash_entry_ptr,
								   struct compaction_properties *properties,
								   struct rocksdb_host_properties_param *host_properties)
{
	/* File Finalize */
	size_t filter_offset, filter_size;
	size_t index_offset, index_size;
	size_t properties_offset, properties_size;
	size_t metaindex_offset, metaindex_size;
	/* Used for metaindex block entry -> properties block handle */
	uint64_t shared, non_shared, value_size;
	char *tmp_buf;
	char encoding_buf[50];

	char trailer[kBlockTrailerSize];
	memset(trailer, 0, kBlockTrailerSize);

	// Write filter block
	if (bits_per_key > 0 && bloom_flag) {
		// build filter block directly in buf_file_out
		filter_offset = output_offset;
		uint64_t len_with_metadata = CalculateSpace(properties->num_entries, bits_per_key);
		int num_probes = GetNumProbes(properties->num_entries, len_with_metadata);

		char *filter_block_ptr = buf_file_out + output_offset;
		memset(filter_block_ptr, 0, len_with_metadata);

		uint32_t len = (uint32_t)(len_with_metadata - kMetadatalen);
		AddAllEntries(filter_block_ptr, len, num_probes,
				properties->num_entries, hash_entry_ptr);

		filter_block_ptr[len] = (char)-1;
		filter_block_ptr[len + 1] = (char)0;
		filter_block_ptr[len + 2] = (char)num_probes;
		// rest of metadata stays zero
		notify_if_slm_data_ready((size_t)buf_file_out + output_offset, len_with_metadata);

		output_offset += len_with_metadata;
		set_xxh3_to_block(filter_block_ptr, len_with_metadata, len_with_metadata, trailer);
		set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)(size_t)trailer, kBlockTrailerSize);
		output_offset += kBlockTrailerSize;

		properties->filter_size = len_with_metadata;
		filter_size = len_with_metadata;
		CSD_DEBUG("finished filter block - %llu %llu\n", filter_offset, filter_size);
	}

	// Write index block
	index_offset = output_offset;
	char *index_buf = buf_file_out + output_offset; // need the whole index block including restart for CRC
	set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)index_block_ptr,
					  index_block_append_ptr - index_block_ptr);
	output_offset += index_block_append_ptr - index_block_ptr;
	set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)index_restart_ptr,
					  index_restart_append_ptr - index_restart_ptr);
	output_offset += index_restart_append_ptr - index_restart_ptr;
	index_size = (index_block_append_ptr - index_block_ptr) +
				 (index_restart_append_ptr - index_restart_ptr);
	set_xxh3_to_block(index_buf, index_size, index_offset, trailer);
	set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)(size_t)trailer, kBlockTrailerSize);
	output_offset += kBlockTrailerSize;
	properties->index_size = index_size + kBlockTrailerSize;
	CSD_DEBUG("finished index block - %llu %llu\n", index_offset, index_size);

	// Write properties block
	properties_offset = output_offset;
	output_offset = __rocksdb_write_properties_block(buf_file_out, output_offset,
													 properties, host_properties);
	properties_size = output_offset - properties_offset;
	notify_if_slm_data_ready((size_t)buf_file_out + properties_offset, properties_size);
	set_xxh3_to_block(buf_file_out + properties_offset, properties_size,
					  properties_offset, trailer);
	set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)(size_t)trailer, kBlockTrailerSize);
	output_offset += kBlockTrailerSize;
	CSD_DEBUG("finished properties block - %llu %llu\n", properties_offset,
			  properties_size);

	// Meta index block
	metaindex_offset = output_offset;
	char *metaindex_buf = buf_file_out + output_offset;
	char *metaindex_append_buf = metaindex_buf;
	uint32_t restart_offset = 0;
	uint32_t num_restarts = 0;

	/* make entry for filter block handle */
	if (bits_per_key > 0 && bloom_flag) {
		shared = 0;
		non_shared = strlen(kFilterBlockName);
		tmp_buf = encoding_buf;
		tmp_buf = PutVarint64(tmp_buf, filter_offset);
		tmp_buf = PutVarint64(tmp_buf, filter_size);
		value_size = (tmp_buf - encoding_buf);
		/* insert entry */
		metaindex_append_buf = EncodeVarint32(metaindex_append_buf, shared);
		metaindex_append_buf = EncodeVarint32(metaindex_append_buf, non_shared);
		metaindex_append_buf = EncodeVarint32(metaindex_append_buf, value_size);
		memcpy(metaindex_append_buf, kFilterBlockName, non_shared);
		metaindex_append_buf += non_shared;
		memcpy(metaindex_append_buf, encoding_buf, value_size);
		metaindex_append_buf += value_size;

		restart_offset = metaindex_append_buf - metaindex_buf;
		num_restarts++;
	}

	/* make entry for properties block handle */
	shared = 0;
	non_shared = strlen(kPropertiesBlockName);
	tmp_buf = encoding_buf;
	tmp_buf = PutVarint64(tmp_buf, properties_offset);
	tmp_buf = PutVarint64(tmp_buf, properties_size);
	value_size = (tmp_buf - encoding_buf);
	/* insert entry */
	metaindex_append_buf = EncodeVarint32(metaindex_append_buf, shared);
	metaindex_append_buf = EncodeVarint32(metaindex_append_buf, non_shared);
	metaindex_append_buf = EncodeVarint32(metaindex_append_buf, value_size);
	memcpy(metaindex_append_buf, kPropertiesBlockName, non_shared);
	metaindex_append_buf += non_shared;
	memcpy(metaindex_append_buf, encoding_buf, value_size);
	metaindex_append_buf += value_size;
	num_restarts++;
	
	/* restarts_ and num_restarts */
	metaindex_append_buf = PutFixed32(metaindex_append_buf, (uint32_t)0);
	if (num_restarts > 1) {
		metaindex_append_buf = PutFixed32(metaindex_append_buf, restart_offset);
	}
	metaindex_append_buf = PutFixed32(metaindex_append_buf, num_restarts);
	metaindex_size = (metaindex_append_buf - metaindex_buf);
	output_offset += metaindex_size;
	notify_if_slm_data_ready((size_t)buf_file_out + metaindex_offset, metaindex_size);
	set_xxh3_to_block(metaindex_buf, metaindex_size, metaindex_offset, trailer);
	set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)(size_t)trailer, kBlockTrailerSize);
	output_offset += kBlockTrailerSize;
	CSD_DEBUG("finished metaindex block - %llu %llu\n", metaindex_offset, metaindex_size);

	// Write footer
	char footer_buf[kNewVersionsEncodedLength];
	memset(footer_buf, 0, kNewVersionsEncodedLength);
	char *footer_append_ptr = footer_buf;
	char *part2, *part3;
	/* Part 1 */
	*(footer_append_ptr++) = 0x4; // kXXH3
	/* Part 2*/
	part2 = footer_append_ptr;
	/* Skip over part 2 */
	footer_append_ptr += kFooterPart2Size;
	/* Part 3 */
	part3 = footer_append_ptr;
	EncodeFixed32(footer_append_ptr, FormatVersion);
	footer_append_ptr += 4;
	EncodeFixed64(footer_append_ptr, kBlockBasedTableMagicNumber);
	/* Start populating Part 2 */
	part2 = PutVarint64(part2, metaindex_offset);
	part2 = PutVarint64(part2, metaindex_size);
	part2 = PutVarint64(part2, index_offset);
	part2 = PutVarint64(part2, index_size);
	set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)footer_buf,
					  kNewVersionsEncodedLength);
	memcpy(buf_file_out + output_offset, footer_buf, kNewVersionsEncodedLength);
	output_offset += kNewVersionsEncodedLength;

	CSD_DEBUG("finish this file with size: %lu\n", output_offset);

	return output_offset;
}

/*
 * __magic_rocksdb_finalized_sstable - Finalize SSTable using circular output buffer
 * @ctx: Circular output buffer context
 * @index_block_ptr: Start of index block data
 * @index_block_append_ptr: End of index block data
 * @index_restart_ptr: Start of index restart array
 * @index_restart_append_ptr: End of index restart array
 * @restart_entry_count: Number of restart entries
 * @bits_per_key: Bloom filter bits per key (0 to disable)
 * @bloom_flag: Whether bloom filter is enabled
 * @hash_entry_ptr: Array of hash values for bloom filter
 * @properties: Compaction properties to write
 * @host_properties: Host-provided properties
 *
 * This is the circular-buffer version of __rocksdb_finalized_sstable.
 * It writes filter block, index block, properties block, meta-index block,
 * and footer to the circular output buffer with proper backpressure handling.
 *
 * Returns: The output offset within the current SSTable (local offset, not logical)
 */
size_t __magic_rocksdb_finalized_sstable(struct circular_output_buf_ctx *ctx,
					 char *index_block_ptr, char *index_block_append_ptr,
					 char *index_restart_ptr, char *index_restart_append_ptr,
					 uint32_t restart_entry_count,
					 int bits_per_key, bool bloom_flag,
					 uint64_t *hash_entry_ptr,
					 struct compaction_properties *properties,
					 struct rocksdb_host_properties_param *host_properties)
{
	/* Track local offset within this SSTable (starts where data blocks ended) */
	size_t output_offset = properties->data_size;

	/* File Finalize */
	size_t filter_offset, filter_size;
	size_t index_offset, index_size;
	size_t properties_offset, properties_size;
	size_t metaindex_offset, metaindex_size;
	/* Used for metaindex block entry -> properties block handle */
	uint64_t shared, non_shared, value_size;
	char *tmp_buf;
	char encoding_buf[50];

	char trailer[kBlockTrailerSize];
	memset(trailer, 0, kBlockTrailerSize);

	// Write filter block
	if (bits_per_key > 0 && bloom_flag) {
		filter_offset = output_offset;
		uint64_t len_with_metadata = CalculateSpace(properties->num_entries, bits_per_key);
		int num_probes = GetNumProbes(properties->num_entries, len_with_metadata);

		/* Use global temp buffer instead of kmalloc */
		memset(g_temp_block_ptr, 0, len_with_metadata);

		uint32_t len = (uint32_t)(len_with_metadata - kMetadatalen);
		AddAllEntries(g_temp_block_ptr, len, num_probes,
			      properties->num_entries, hash_entry_ptr);

		g_temp_block_ptr[len] = (char)-1;
		g_temp_block_ptr[len + 1] = (char)0;
		g_temp_block_ptr[len + 2] = (char)num_probes;
		/* rest of metadata stays zero */

		/* Write filter block to circular buffer */
		circular_output_set_data(ctx, g_temp_block_ptr, len_with_metadata);
		output_offset += len_with_metadata;

		/* Compute XXH3 and write trailer */
		set_xxh3_to_block(g_temp_block_ptr, len_with_metadata, filter_offset, trailer);
		circular_output_set_data(ctx, trailer, kBlockTrailerSize);
		output_offset += kBlockTrailerSize;

		properties->filter_size = len_with_metadata;
		filter_size = len_with_metadata;
		CSD_DEBUG_MAGIC_COMPACTION("finished filter block - %lu %lu\n", filter_offset, filter_size);
	}

	// Write index block
	index_offset = output_offset;
	size_t index_entries_size = index_block_append_ptr - index_block_ptr;
	size_t index_restart_size = index_restart_append_ptr - index_restart_ptr;
	index_size = index_entries_size + index_restart_size;

	/* Append restart array after index entries in g_index_block_ptr (no temp alloc needed) */
	memcpy(index_block_append_ptr, index_restart_ptr, index_restart_size);

	/* Write index block to circular buffer from g_index_block_ptr */
	circular_output_set_data(ctx, index_block_ptr, index_size);
	output_offset += index_size;

	/* Compute XXH3 and write trailer */
	set_xxh3_to_block(index_block_ptr, index_size, index_offset, trailer);
	circular_output_set_data(ctx, trailer, kBlockTrailerSize);
	output_offset += kBlockTrailerSize;

	properties->index_size = index_size + kBlockTrailerSize;
	CSD_DEBUG_MAGIC_COMPACTION("finished index block - %lu %lu\n", index_offset, index_size);

	// Write properties block
	properties_offset = output_offset;

	/* Reuse g_temp_block_ptr for properties (filter is already written) */
	size_t props_end = __rocksdb_write_properties_block(g_temp_block_ptr, 0,
							    properties, host_properties);
	properties_size = props_end;

	/* Write to circular buffer (handles wrap-around and ghost page mirroring) */
	circular_output_set_data(ctx, g_temp_block_ptr, properties_size);
	output_offset += properties_size;

	/* Compute XXH3 and write trailer */
	set_xxh3_to_block(g_temp_block_ptr, properties_size, properties_offset, trailer);
	circular_output_set_data(ctx, trailer, kBlockTrailerSize);
	output_offset += kBlockTrailerSize;

	CSD_DEBUG_MAGIC_COMPACTION("finished properties block - %lu %lu\n", properties_offset, properties_size);

	// Meta index block
	metaindex_offset = output_offset;

	/* Reuse g_temp_block_ptr for metaindex (properties is already written) */
	char *metaindex_buf = g_temp_block_ptr;
	char *metaindex_append_buf = metaindex_buf;
	uint32_t restart_offset_meta = 0;
	uint32_t num_restarts = 0;

	/* make entry for filter block handle */
	if (bits_per_key > 0 && bloom_flag) {
		shared = 0;
		non_shared = strlen(kFilterBlockName);
		tmp_buf = encoding_buf;
		tmp_buf = PutVarint64(tmp_buf, filter_offset);
		tmp_buf = PutVarint64(tmp_buf, filter_size);
		value_size = (tmp_buf - encoding_buf);
		/* insert entry */
		metaindex_append_buf = EncodeVarint32(metaindex_append_buf, shared);
		metaindex_append_buf = EncodeVarint32(metaindex_append_buf, non_shared);
		metaindex_append_buf = EncodeVarint32(metaindex_append_buf, value_size);
		memcpy(metaindex_append_buf, kFilterBlockName, non_shared);
		metaindex_append_buf += non_shared;
		memcpy(metaindex_append_buf, encoding_buf, value_size);
		metaindex_append_buf += value_size;

		restart_offset_meta = metaindex_append_buf - metaindex_buf;
		num_restarts++;
	}

	/* make entry for properties block handle */
	shared = 0;
	non_shared = strlen(kPropertiesBlockName);
	tmp_buf = encoding_buf;
	tmp_buf = PutVarint64(tmp_buf, properties_offset);
	tmp_buf = PutVarint64(tmp_buf, properties_size);
	value_size = (tmp_buf - encoding_buf);
	/* insert entry */
	metaindex_append_buf = EncodeVarint32(metaindex_append_buf, shared);
	metaindex_append_buf = EncodeVarint32(metaindex_append_buf, non_shared);
	metaindex_append_buf = EncodeVarint32(metaindex_append_buf, value_size);
	memcpy(metaindex_append_buf, kPropertiesBlockName, non_shared);
	metaindex_append_buf += non_shared;
	memcpy(metaindex_append_buf, encoding_buf, value_size);
	metaindex_append_buf += value_size;
	num_restarts++;

	/* restarts_ and num_restarts */
	metaindex_append_buf = PutFixed32(metaindex_append_buf, (uint32_t)0);
	if (num_restarts > 1) {
		metaindex_append_buf = PutFixed32(metaindex_append_buf, restart_offset_meta);
	}
	metaindex_append_buf = PutFixed32(metaindex_append_buf, num_restarts);
	metaindex_size = (metaindex_append_buf - metaindex_buf);

	/* Write metaindex block to circular buffer */
	circular_output_set_data(ctx, metaindex_buf, metaindex_size);
	output_offset += metaindex_size;

	/* Compute XXH3 and write trailer */
	set_xxh3_to_block(metaindex_buf, metaindex_size, metaindex_offset, trailer);
	circular_output_set_data(ctx, trailer, kBlockTrailerSize);
	output_offset += kBlockTrailerSize;

	CSD_DEBUG_MAGIC_COMPACTION("finished metaindex block - %lu %lu\n", metaindex_offset, metaindex_size);

	// Write footer
	char footer_buf[kNewVersionsEncodedLength];
	memset(footer_buf, 0, kNewVersionsEncodedLength);
	char *footer_append_ptr = footer_buf;
	char *part2;
	/* Part 1 */
	*(footer_append_ptr++) = 0x4; // kXXH3
	/* Part 2*/
	part2 = footer_append_ptr;
	/* Skip over part 2 for now */
	footer_append_ptr += kFooterPart2Size;
	/* Part 3 */
	EncodeFixed32(footer_append_ptr, FormatVersion);
	footer_append_ptr += 4;
	EncodeFixed64(footer_append_ptr, kBlockBasedTableMagicNumber);
	/* Start populating Part 2 */
	part2 = PutVarint64(part2, metaindex_offset);
	part2 = PutVarint64(part2, metaindex_size);
	part2 = PutVarint64(part2, index_offset);
	part2 = PutVarint64(part2, index_size);

	/* Write footer to circular buffer */
	circular_output_set_data(ctx, footer_buf, kNewVersionsEncodedLength);
	output_offset += kNewVersionsEncodedLength;

	CSD_DEBUG_MAGIC_COMPACTION("finish this SSTable with size: %lu\n", output_offset);

	return output_offset;
}

void __rocksdb_handle_index_block(struct decode_info *index_info,
								  char **index_block_append_ptr,
								  char **index_restart_append_ptr,
								  uint32_t *restart_entry_count, size_t output_offset,
								  size_t indexblock_offset, size_t block_size)
{
	/* Index block entry handling */
	char handle_encoding[20];
	char *handle_encoding_ptr = handle_encoding;
	handle_encoding_ptr = PutVarint64(handle_encoding_ptr, output_offset);
	handle_encoding_ptr = PutVarint64(handle_encoding_ptr, block_size);
	int index_value_size = handle_encoding_ptr - handle_encoding;
	char *start = *index_block_append_ptr;

	uint32_t non_shared_without_seqno = index_info->non_shared - kNumInternalBytes;
	*index_block_append_ptr = PutVarint32(*index_block_append_ptr, index_info->shared);
	*index_block_append_ptr =
		PutVarint32(*index_block_append_ptr, non_shared_without_seqno);
	*index_block_append_ptr = PutVarint32(*index_block_append_ptr, index_value_size);
	memcpy(*index_block_append_ptr, index_info->key, non_shared_without_seqno);
	*index_block_append_ptr += non_shared_without_seqno;
	memcpy(*index_block_append_ptr, handle_encoding, index_value_size);
	*index_block_append_ptr += index_value_size;

	// restart about index block should use indexblock_offset, not output_offset
	*index_restart_append_ptr = PutFixed32(*index_restart_append_ptr, indexblock_offset);
	(*restart_entry_count)++;

	return;
}

size_t __rocksdb_compaction(void *buf_in, void *buf_out, size_t size, void *param)
{
	size_t result = 0;
	struct CSD_PARAMS *temp = (struct CSD_PARAMS *)param;
	struct decode_info info_first, info_second;
	int cmp_result = 0;
	int pid = temp->profile_info.pid;
	int host_id = temp->profile_info.host_id;
	size_t sstable_size = temp->rocksdb_compaction_params.sstable_size;
	size_t datablock_threshold = temp->rocksdb_compaction_params.datablock_threshold;
	int sst_block_size = temp->rocksdb_compaction_params.block_size;
	bool crc_flag = temp->rocksdb_compaction_params.crc_flag;
	bool bloom_flag = temp->rocksdb_compaction_params.bloom_flag;
	int bits_per_key = temp->rocksdb_compaction_params.bits_per_key;

	char *first_level = (char *)buf_in;
	char *second_level = first_level + temp->rocksdb_compaction_params.second_level_start;

	char *first_limit = first_level + temp->rocksdb_compaction_params.first_level_size;
	char *second_limit = second_level + temp->rocksdb_compaction_params.second_level_size;

	char *first_level_starting_offset = first_level; // for SLM request timing calculation
	char *second_level_starting_offset =
		second_level; // for SLM request timing calculation
	size_t output_offset = 0;

	char *tmp_first;
	char *tmp_second;

	size_t *file_size = (size_t *)((char *)buf_out + sizeof(int));
	struct compaction_stats *stats =
		(struct compaction_stats *)((char *)buf_out + sizeof(int) +
									sizeof(size_t) * MAX_OUTPUT_TABLES);
	struct compacted_file_metadata *meta =
		(struct compacted_file_metadata *)((char *)buf_out + sizeof(int) +
										   sizeof(size_t) * MAX_OUTPUT_TABLES +
										   sizeof(struct compaction_stats));
	int file_size_count = 0;
	void *buf_file_meta = buf_out;
	buf_out = buf_out + FRONT_METADATA_SIZE; // room for file size metadata

	void *buf_file_out = buf_out;
	size_t block_size = 0;
	char *index_block_ptr = g_index_block_ptr;
	char *index_block_append_ptr = index_block_ptr;
	struct decode_info *index_info;
	char *index_restart_ptr = g_index_restart_ptr;
	char *index_restart_append_ptr = index_restart_ptr;
	uint32_t restart_entry_count = 0;
	uint64_t *hash_entry_ptr = g_hash_entry_ptr;

	char trailer[kBlockTrailerSize];
	memset(trailer, 0, kBlockTrailerSize);

	unsigned char restart_inverval[8] = {
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
	};
	uint64_t interval_value, comp_value;
	memcpy(&interval_value, restart_inverval, sizeof(uint64_t));

	struct compaction_properties properties;
	memset(&properties, 0, sizeof(struct compaction_properties));
	uint64_t seqno;
	memset(stats, 0, sizeof(struct compaction_stats));

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	u64 compaction_start = ktime_get_ns();
#endif

	NVMEV_CSD_PROFILE_REAL_START(pid, host_id);
	CSD_DEBUG("Rocksdb compaction is about to begin withc crc flag %d\n", crc_flag);
	CSD_DEBUG("%llu %llu %llu(%llu)\n", temp->rocksdb_compaction_params.first_level_size,
			  temp->rocksdb_compaction_params.second_level_size, sstable_size,
			  datablock_threshold);

	/* 
	 * Due to encoding, entry size might not be aligned with SLM_PAGE_SIZE.
	 * Therefore, we will fetch one next SLM page before accessing the SLM page.
	 */
	check_data_using_ptr((size_t)first_level, SLM_PAGE_SIZE, pid, host_id);
	check_data_using_ptr((size_t)second_level, SLM_PAGE_SIZE, pid, host_id);

	size_t first_level_slm_page_num =
		get_slm_offset((size_t)first_level) / SLM_PAGE_SIZE; // SLM loaded page number
	size_t second_level_slm_page_num = get_slm_offset((size_t)second_level) / SLM_PAGE_SIZE;
	size_t last_first_level_slm_page_num = get_slm_offset((size_t)first_limit) / SLM_PAGE_SIZE;
	size_t last_second_level_slm_page_num = get_slm_offset((size_t)second_limit) / SLM_PAGE_SIZE;

	char *prev_first_level = first_level;
	char *prev_second_level = second_level;

	int cumulative_first_level_block_size = 0;
	int cumulative_second_level_block_size = 0;
	int cumulative_output_block_size = 0;
	char *prev_tmp_first;
	char *prev_tmp_second;
	size_t output_block_offset = 0;

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	u64 single_start = ktime_get_ns();
#endif
	bool adjust_block_size_flag = false; // used to determine block completion

	/* Base on the notion that we know we can skip the BlockTrailer */
	while (first_level < first_limit && second_level < second_limit) {
		tmp_first = first_level;
		tmp_second = second_level;

		if (get_slm_offset((size_t)prev_first_level) / SLM_PAGE_SIZE !=
			get_slm_offset((size_t)first_level) / SLM_PAGE_SIZE) {
			notify_compute_ready((size_t)prev_first_level);
			prev_first_level = first_level;
		}
		if (get_slm_offset((size_t)prev_second_level) / SLM_PAGE_SIZE !=
			get_slm_offset((size_t)second_level) / SLM_PAGE_SIZE) {
			notify_compute_ready((size_t)prev_second_level);
			prev_second_level = second_level;
		}

		if ((get_slm_offset((size_t)first_level) / SLM_PAGE_SIZE) + 1 !=
				first_level_slm_page_num &&
			first_level_slm_page_num + 1 < last_first_level_slm_page_num) {
			check_data_using_ptr((size_t)first_level + SLM_PAGE_SIZE, SLM_PAGE_SIZE, pid,
								 host_id);
			first_level_slm_page_num++;
		}
		if ((get_slm_offset((size_t)second_level) / SLM_PAGE_SIZE) + 1 !=
				second_level_slm_page_num &&
			second_level_slm_page_num + 1 < last_second_level_slm_page_num) {
			check_data_using_ptr((size_t)second_level + SLM_PAGE_SIZE, SLM_PAGE_SIZE, pid,
								 host_id);
			second_level_slm_page_num++;
		}

		comp_value = *(uint64_t *)first_level;
		if (comp_value == interval_value) {
			// skip the restart entry
			first_level = first_level + 8 + kBlockTrailerSize;
			cumulative_first_level_block_size = 0;
			continue;
		}
		comp_value = *(uint64_t *)second_level;
		if (comp_value == interval_value) {
			// skip the restart entry
			second_level = second_level + 8 + kBlockTrailerSize;
			cumulative_second_level_block_size = 0;
			continue;
		}

		prev_tmp_first = tmp_first;
		tmp_first = DecodeEntry(tmp_first, first_limit, &info_first.shared,
								&info_first.non_shared, &info_first.value_length);
		memcpy(info_first.key, tmp_first, info_first.non_shared);
		tmp_first = tmp_first + info_first.non_shared + info_first.value_length;

		prev_tmp_second = tmp_second;
		tmp_second = DecodeEntry(tmp_second, second_limit, &info_second.shared,
								 &info_second.non_shared, &info_second.value_length);
		memcpy(info_second.key, tmp_second, info_second.non_shared);
		tmp_second = tmp_second + info_second.non_shared + info_second.value_length;

		if (info_first.non_shared != 24) {
			printk("First level key length is not 24: %d\n", info_first.non_shared);
			return 0;
		}
		if (info_second.non_shared != 24) {
			printk("Second level key length is not 24: %d\n", info_second.non_shared);
			return 0;
		}

		if (info_first.non_shared != info_second.non_shared) {
			printk("%ld %ld %ld %ld\n",
				   (prev_tmp_first - first_level_starting_offset),
				   (prev_tmp_second - second_level_starting_offset),
				   (tmp_first - prev_tmp_first), (tmp_second - prev_tmp_second));
			NVMEV_ERROR("Key length mismatch: %d %d\n", info_first.non_shared,
						info_second.non_shared);
			BUG();
		}
		cmp_result = memcmp(info_first.key, info_second.key,
							(info_first.non_shared - kNumInternalBytes));

		/* Copy KV pair (except CRC) depending on the cmp_result */
		if (cmp_result == 0) {
			// key is identical
			block_size = (tmp_first - first_level);
			set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)first_level,
							  block_size);
			index_info = &info_first;

			cumulative_first_level_block_size += (tmp_first - prev_tmp_first);
			if (cumulative_first_level_block_size >= sst_block_size) {
				cumulative_first_level_block_size = 0;
				tmp_first = tmp_first + 8 + kBlockTrailerSize; // Move to next block
			}

			cumulative_second_level_block_size += (tmp_second - prev_tmp_second);
			if (cumulative_second_level_block_size >= sst_block_size) {
				cumulative_second_level_block_size = 0;
				tmp_second = tmp_second + 8 + kBlockTrailerSize; // Move to next block
			}

			first_level = tmp_first;
			second_level = tmp_second;

			stats->num_input_entries += 2;
			stats->raw_input_key_size += (info_first.shared + info_first.non_shared);
			stats->raw_input_key_size += (info_second.shared + info_second.non_shared);
			stats->raw_input_value_size +=
				(info_first.value_length + info_second.value_length);
		} else if (cmp_result < 0) {
			// level 1 is smaller
			block_size = (tmp_first - first_level);
			set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)first_level,
							  block_size);
			index_info = &info_first;

			cumulative_first_level_block_size += (tmp_first - prev_tmp_first);
			if (cumulative_first_level_block_size >= sst_block_size) {
				cumulative_first_level_block_size = 0;
				tmp_first = tmp_first + 8 + kBlockTrailerSize; // Move to next block
			}

			first_level = tmp_first;

			stats->num_input_entries++;
			stats->raw_input_key_size += (info_first.shared + info_first.non_shared);
			stats->raw_input_value_size += info_first.value_length;
		} else {
			// level 2 is smaller
			block_size = (tmp_second - second_level);
			set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)second_level,
							  block_size);
			index_info = &info_second;

			cumulative_second_level_block_size += (tmp_second - prev_tmp_second);
			if (cumulative_second_level_block_size >= sst_block_size) {
				cumulative_second_level_block_size = 0;
				tmp_second = tmp_second + 8 + kBlockTrailerSize; // Move to next block
			}

			second_level = tmp_second;

			stats->num_input_entries++;
			stats->raw_input_key_size += (info_second.shared + info_second.non_shared);
			stats->raw_input_value_size += info_second.value_length;
		}
		stats->num_output_entries++;
		
		if (bits_per_key > 0 && bloom_flag) {
			hash_entry_ptr[properties.num_entries] = 
				XXPH3_64bits(index_info->key, index_info->non_shared - kNumInternalBytes);
		}

		/* Update Properties and FileMetadata*/
		seqno =
			DecodeFixed64(index_info->key + index_info->non_shared - kNumInternalBytes) >>
			8;
		if (output_offset == 0) {
			memcpy(meta[file_size_count].smallest_key, index_info->key,
				   index_info->non_shared); // smallest Key
			meta[file_size_count].smallest_klen = index_info->non_shared;
			/* Init seqno */
			meta[file_size_count].smallest_seqno = seqno;
			meta[file_size_count].largest_seqno = seqno;
		}
		if (seqno < meta[file_size_count].smallest_seqno) {
			meta[file_size_count].smallest_seqno = seqno;
		}
		if (seqno > meta[file_size_count].largest_seqno) {
			meta[file_size_count].largest_seqno = seqno;
		}

		/* Handle output block */
		if (adjust_block_size_flag == false && (block_size < 1024)) {
			sst_block_size -= block_size;
			adjust_block_size_flag = true;
		}
		cumulative_output_block_size += block_size;
		if (cumulative_output_block_size >= sst_block_size) {
			set_data_from_ptr((size_t)buf_file_out + output_block_offset +
								  cumulative_output_block_size,
							  (size_t)restart_inverval, 8);
			cumulative_output_block_size += 8; // add restart size

			__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
										 &index_restart_append_ptr, &restart_entry_count,
										 output_block_offset,
										 (index_block_append_ptr - index_block_ptr),
										 cumulative_output_block_size);

			/* Calculate CRC */
			if (crc_flag) {
				set_xxh3_to_block((char *)buf_file_out + output_block_offset,
								  cumulative_output_block_size, output_block_offset,
								  trailer);
			}
			set_data_from_ptr((size_t)buf_file_out + output_block_offset +
								  cumulative_output_block_size,
							  (size_t)trailer, kBlockTrailerSize);
			output_offset += block_size;
			output_offset += 8 + kBlockTrailerSize; // Move to next block

			output_block_offset = output_offset;
			cumulative_output_block_size = 0;
			__update_compaction_properties(&properties, index_info, true);
		} else {
			output_offset += block_size;
			__update_compaction_properties(&properties, index_info, false);
		}

		if (output_offset >= datablock_threshold) {
			CSD_DEBUG("Data block end\n");
			/* Finish last data block */
			if (cumulative_output_block_size > 0) {
				set_data_from_ptr((size_t)buf_file_out + output_block_offset +
									  cumulative_output_block_size,
								  (size_t)restart_inverval, 8);
				cumulative_output_block_size += 8; // add restart size
				__update_compaction_properties(&properties, NULL, true);

				__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
											 &index_restart_append_ptr,
											 &restart_entry_count, output_block_offset,
											 (index_block_append_ptr - index_block_ptr),
											 cumulative_output_block_size);

				/* Calculate CRC */
				if (crc_flag) {
					set_xxh3_to_block((char *)buf_file_out + output_block_offset,
									  cumulative_output_block_size, output_block_offset,
									  trailer);
				}
				set_data_from_ptr((size_t)buf_file_out + output_block_offset +
									  cumulative_output_block_size,
								  (size_t)trailer, kBlockTrailerSize);
				output_offset += 8 + kBlockTrailerSize; // Move to next block

				output_block_offset = output_offset;
				cumulative_output_block_size = 0;
			}

			memcpy(meta[file_size_count].largest_key, index_info->key,
				   index_info->non_shared); // largest Key
			meta[file_size_count].largest_klen = index_info->non_shared;
			properties.data_size = output_offset;
			properties.tail_start_offset = output_offset;

			index_restart_append_ptr =
				PutFixed32(index_restart_append_ptr,
						   restart_entry_count); // append entry count to index_restart
			CSD_DEBUG("data block size: %lu\n", output_offset);
			CSD_DEBUG("index block size: %lu\n",
					  index_block_append_ptr - index_block_ptr);
			CSD_DEBUG("index restart size: %lu (entry: %d)\n",
					  index_restart_append_ptr - index_restart_ptr, restart_entry_count);

			output_offset = __rocksdb_finalized_sstable(
				buf_file_out, output_offset, index_block_ptr, index_block_append_ptr,
				index_restart_ptr, index_restart_append_ptr, restart_entry_count,
				bits_per_key, bloom_flag, hash_entry_ptr,
				&properties, &(temp->rocksdb_compaction_params.host_properties));

			if (output_offset > sstable_size) {
				NVMEV_ERROR("BUG!!! Large SSTable size %lu\n", output_offset);
				BUG();
			}

			// Fill padding until max_file_size, No need to actually perform zero-padding
			notify_if_slm_data_ready((size_t)buf_file_out + output_offset,
									 sstable_size - output_offset);

			meta[file_size_count].tail_size =
				output_offset - properties.tail_start_offset;

			// Reset metadata
			memcpy(&(meta[file_size_count].properties), &properties,
				   sizeof(struct compaction_properties));
			memset(&properties, 0, sizeof(struct compaction_properties));
			file_size[file_size_count++] = output_offset;

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
			{
				u64 single_end = ktime_get_ns();
				u64 elapsed_us = (single_end - single_start) / 1000;
				printk(KERN_INFO "[COMPACTION] single time_us=%llu\n", elapsed_us);
				single_start = ktime_get_ns();
			}
#endif

			buf_file_out = buf_out + sstable_size * file_size_count;
			output_offset = 0;
			output_block_offset = 0;
			cumulative_output_block_size = 0;
			memset(index_block_ptr, 0, 1024 * 1024);
			index_block_append_ptr = index_block_ptr;
			memset(index_restart_ptr, 0, 1024 * 1024);
			index_restart_append_ptr = index_restart_ptr;
			memset(hash_entry_ptr, 0, 128 * 1024);
			restart_entry_count = 0;
			CSD_DEBUG("Data block end end\n");
		}
	}

	if (get_slm_offset((size_t)prev_first_level) / SLM_PAGE_SIZE !=
		get_slm_offset((size_t)first_level) / SLM_PAGE_SIZE) {
		notify_compute_ready((size_t)prev_first_level);
		prev_first_level = first_level;
	}
	if (get_slm_offset((size_t)prev_second_level) / SLM_PAGE_SIZE !=
		get_slm_offset((size_t)second_level) / SLM_PAGE_SIZE) {
		notify_compute_ready((size_t)prev_second_level);
		prev_second_level = second_level;
	}

	if (second_level < second_limit) {
		first_level = second_level;
		first_limit = second_limit;
		first_level_slm_page_num = second_level_slm_page_num;
		last_first_level_slm_page_num = last_second_level_slm_page_num;
		cumulative_first_level_block_size = cumulative_second_level_block_size;

		prev_first_level = first_level;
		notify_compute_ready((size_t)prev_first_level);
	} else {
		prev_second_level = second_level;
		notify_compute_ready((size_t)prev_second_level);
	}

	prev_first_level = first_level;

	while (first_level < first_limit) {
		tmp_first = first_level;

		if (get_slm_offset((size_t)prev_first_level) / SLM_PAGE_SIZE !=
			get_slm_offset((size_t)first_level) / SLM_PAGE_SIZE) {
			notify_compute_ready((size_t)prev_first_level);
			prev_first_level = first_level;
		}

		if ((get_slm_offset((size_t)first_level) / SLM_PAGE_SIZE) + 1 !=
				first_level_slm_page_num &&
			first_level_slm_page_num + 1 < last_first_level_slm_page_num) {
			check_data_using_ptr((size_t)first_level + SLM_PAGE_SIZE, SLM_PAGE_SIZE, pid,
								 host_id);
			first_level_slm_page_num++;
		}

		if ((first_limit - first_level) == (8 + kBlockTrailerSize)) {
			first_level = first_limit;
			break;
		}

		prev_tmp_first = tmp_first;
		tmp_first = DecodeEntry(tmp_first, first_limit, &info_first.shared,
								&info_first.non_shared, &info_first.value_length);
		memcpy(info_first.key, tmp_first, info_first.non_shared);
		tmp_first = tmp_first + info_first.non_shared + info_first.value_length;

		block_size = (tmp_first - first_level);
		set_data_from_ptr((size_t)buf_file_out + output_offset, (size_t)first_level,
						  block_size);
		index_info = &info_first;

		cumulative_first_level_block_size += (tmp_first - prev_tmp_first);
		if (cumulative_first_level_block_size >= sst_block_size) {
			cumulative_first_level_block_size = 0;
			tmp_first = tmp_first + 8 + kBlockTrailerSize; // Move to next block
		}

		first_level = tmp_first;

		stats->num_input_entries++;
		stats->raw_input_key_size += (index_info->shared + index_info->non_shared);
		stats->raw_input_value_size += index_info->value_length;
		stats->num_output_entries++;
		
		if (bits_per_key > 0 && bloom_flag) {
			hash_entry_ptr[properties.num_entries] = 
				XXPH3_64bits(index_info->key, index_info->non_shared - kNumInternalBytes);
		}

		/* Update Properties and FileMetadata*/
		seqno =
			DecodeFixed64(index_info->key + index_info->non_shared - kNumInternalBytes) >>
			8;
		if (output_offset == 0) {
			memcpy(meta[file_size_count].smallest_key, index_info->key,
				   index_info->non_shared); // smallest Key
			meta[file_size_count].smallest_klen = index_info->non_shared;
			/* Init seqno */
			meta[file_size_count].smallest_seqno = seqno;
			meta[file_size_count].largest_seqno = seqno;
		}
		if (seqno < meta[file_size_count].smallest_seqno) {
			meta[file_size_count].smallest_seqno = seqno;
		}
		if (seqno > meta[file_size_count].largest_seqno) {
			meta[file_size_count].largest_seqno = seqno;
		}

		/* Handle output block */
		cumulative_output_block_size += block_size;
		if (cumulative_output_block_size >= sst_block_size) {
			set_data_from_ptr((size_t)buf_file_out + output_block_offset +
								  cumulative_output_block_size,
							  (size_t)restart_inverval, 8);
			cumulative_output_block_size += 8; // add restart size

			__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
										 &index_restart_append_ptr, &restart_entry_count,
										 output_block_offset,
										 (index_block_append_ptr - index_block_ptr),
										 cumulative_output_block_size);

			/* Calculate CRC */
			if (crc_flag) {
				set_xxh3_to_block((char *)buf_file_out + output_block_offset,
								  cumulative_output_block_size, output_block_offset,
								  trailer);
			}
			set_data_from_ptr((size_t)buf_file_out + output_block_offset +
								  cumulative_output_block_size,
							  (size_t)trailer, kBlockTrailerSize);
			output_offset += block_size;
			output_offset += 8 + kBlockTrailerSize; // Move to next block

			output_block_offset = output_offset;
			cumulative_output_block_size = 0;
			__update_compaction_properties(&properties, index_info, true);
		} else {
			output_offset += block_size;
			__update_compaction_properties(&properties, index_info, false);
		}

		if (output_offset >= datablock_threshold) {
			/* Finish last data block */
			if (cumulative_output_block_size > 0) {
				set_data_from_ptr((size_t)buf_file_out + output_block_offset +
									  cumulative_output_block_size,
								  (size_t)restart_inverval, 8);
				cumulative_output_block_size += 8; // add restart size
				__update_compaction_properties(&properties, NULL, true);

				__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
											 &index_restart_append_ptr,
											 &restart_entry_count, output_block_offset,
											 (index_block_append_ptr - index_block_ptr),
											 cumulative_output_block_size);

				/* Calculate CRC */
				if (crc_flag) {
					set_xxh3_to_block((char *)buf_file_out + output_block_offset,
									  cumulative_output_block_size, output_block_offset,
									  trailer);
				}
				set_data_from_ptr((size_t)buf_file_out + output_block_offset +
									  cumulative_output_block_size,
								  (size_t)trailer, kBlockTrailerSize);
				output_offset += 8 + kBlockTrailerSize; // Move to next block

				output_block_offset = output_offset;
				cumulative_output_block_size = 0;
			}

			memcpy(meta[file_size_count].largest_key, index_info->key,
				   index_info->non_shared); // largest Key
			meta[file_size_count].largest_klen = index_info->non_shared;
			properties.data_size = output_offset;
			properties.tail_start_offset = output_offset;

			index_restart_append_ptr =
				PutFixed32(index_restart_append_ptr,
						   restart_entry_count); // append entry count to index_restart
			CSD_DEBUG("data block size: %lu\n", output_offset);
			CSD_DEBUG("index block size: %lu\n",
					  index_block_append_ptr - index_block_ptr);
			CSD_DEBUG("index restart size: %lu (entry: %d)\n",
					  index_restart_append_ptr - index_restart_ptr, restart_entry_count);

			output_offset = __rocksdb_finalized_sstable(
				buf_file_out, output_offset, index_block_ptr, index_block_append_ptr,
				index_restart_ptr, index_restart_append_ptr, restart_entry_count,
				bits_per_key, bloom_flag, hash_entry_ptr,
				&properties, &(temp->rocksdb_compaction_params.host_properties));
			
			if (output_offset > sstable_size) {
				NVMEV_ERROR("BUG!!! Large SSTable size\n");
				BUG();
			}

			// Fill padding until max_file_size, No need to actually perform zero-padding
			notify_if_slm_data_ready((size_t)buf_file_out + output_offset,
									 sstable_size - output_offset);

			meta[file_size_count].tail_size =
				output_offset - properties.tail_start_offset;

			// Reset metadata
			memcpy(&(meta[file_size_count].properties), &properties,
				   sizeof(struct compaction_properties));
			memset(&properties, 0, sizeof(struct compaction_properties));
			file_size[file_size_count++] = output_offset;


#if (_MAGIC_TIME_BREAKDOWN_ == 1)
			{
				u64 single_end = ktime_get_ns();
				u64 elapsed_us = (single_end - single_start) / 1000;
				printk(KERN_INFO "[COMPACTION] single time_us=%llu\n", elapsed_us);
				single_start = ktime_get_ns();
			}
#endif

			buf_file_out = buf_out + sstable_size * file_size_count;
			output_offset = 0;
			output_block_offset = 0;
			cumulative_output_block_size = 0;
			memset(index_block_ptr, 0, 1024 * 1024);
			index_block_append_ptr = index_block_ptr;
			memset(index_restart_ptr, 0, 1024 * 1024);
			index_restart_append_ptr = index_restart_ptr;
			memset(hash_entry_ptr, 0, 128 * 1024);
			restart_entry_count = 0;
		}
	}

	if (get_slm_offset((size_t)prev_first_level) / SLM_PAGE_SIZE !=
		get_slm_offset((size_t)first_level) / SLM_PAGE_SIZE) {
		notify_compute_ready((size_t)prev_first_level);
		prev_first_level = first_level;
	}

	prev_first_level = first_level;
	notify_compute_ready((size_t)prev_first_level);

	if (output_offset > 0) {
		/* Finish last data block */
		if (cumulative_output_block_size > 0) {
			set_data_from_ptr((size_t)buf_file_out + output_block_offset +
								  cumulative_output_block_size,
							  (size_t)restart_inverval, 8);
			cumulative_output_block_size += 8; // add restart size
			__update_compaction_properties(&properties, NULL, true);

			__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
										 &index_restart_append_ptr, &restart_entry_count,
										 output_block_offset,
										 (index_block_append_ptr - index_block_ptr),
										 cumulative_output_block_size);

			/* Calculate CRC */
			if (crc_flag) {
				set_xxh3_to_block((char *)buf_file_out + output_block_offset,
								  cumulative_output_block_size, output_block_offset,
								  trailer);
			}
			set_data_from_ptr((size_t)buf_file_out + output_block_offset +
								  cumulative_output_block_size,
							  (size_t)trailer, kBlockTrailerSize);
			output_offset += 8 + kBlockTrailerSize; // Move to next block

			output_block_offset = output_offset;
			cumulative_output_block_size = 0;
		}

		memcpy(meta[file_size_count].largest_key, index_info->key,
			   index_info->non_shared); // largest Key
		meta[file_size_count].largest_klen = index_info->non_shared;
		properties.data_size = output_offset;
		properties.tail_start_offset = output_offset;

		index_restart_append_ptr =
			PutFixed32(index_restart_append_ptr,
					   restart_entry_count); // append entry count to index_restart
		CSD_DEBUG("data block size: %lu\n", output_offset);
		CSD_DEBUG("index block size: %lu\n", index_block_append_ptr - index_block_ptr);
		CSD_DEBUG("index restart size: %lu (entry: %d)\n",
				  index_restart_append_ptr - index_restart_ptr, restart_entry_count);

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
		{
			u64 single_end = ktime_get_ns();
			u64 elapsed_us = (single_end - single_start) / 1000;
			printk(KERN_INFO "[COMPACTION] single time_us=%llu\n", elapsed_us);
			single_start = ktime_get_ns();
		}
#endif

		output_offset = __rocksdb_finalized_sstable(
			buf_file_out, output_offset, index_block_ptr, index_block_append_ptr,
			index_restart_ptr, index_restart_append_ptr, restart_entry_count,
			bits_per_key, bloom_flag, hash_entry_ptr,
			&properties, &(temp->rocksdb_compaction_params.host_properties));
		
		meta[file_size_count].tail_size = output_offset - properties.tail_start_offset;

		memcpy(&(meta[file_size_count].properties), &properties,
			   sizeof(struct compaction_properties));
		file_size[file_size_count++] = output_offset;
	}

	memcpy(buf_file_meta, &file_size_count, sizeof(int));
	notify_if_slm_data_ready((size_t)buf_file_meta, FRONT_METADATA_SIZE);

	NVMEV_CSD_PROFILE_REAL_END(pid, host_id);

#ifdef CONFIG_CSD_FUNC_VERBOSE
	for (int i = 0; i < file_size_count; i++)
		CSD_DEBUG("%d - %llu\n", i, file_size[i]);
#endif

	size_t total_io_size = 0;
	if (output_offset > 0) {
		// last file is small
		total_io_size =
			((file_size_count - 1) * sstable_size) + file_size[file_size_count - 1];
	} else {
		total_io_size = ((file_size_count)*sstable_size);
	}

#if (SUPPORT_ASYNC_COMPUTE == 1)
	total_io_size += FRONT_METADATA_SIZE;
#endif

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	{
		u64 compaction_end = ktime_get_ns();
		u64 elapsed_us = (compaction_end - compaction_start) / 1000;
		printk(KERN_INFO "[COMPACTION] time_us=%llu\n", elapsed_us);
	}
#endif

	return total_io_size;
}

size_t __magic_rocksdb_compaction(void *param)
{
	struct magic_params *temp = (struct magic_params *)param;
	struct decode_info info_first, info_second;
	int cmp_result = 0;
	int pid = 0;
	int host_id = 0;
	size_t sstable_size = temp->compaction.sstable_size;
	size_t datablock_threshold = temp->compaction.datablock_threshold;
	int sst_block_size = temp->compaction.block_size;
	bool crc_flag = temp->compaction.crc_flag;
	bool bloom_flag = temp->compaction.bloom_flag;
	int bits_per_key = temp->compaction.bits_per_key;

	/* Initialize circular buffer contexts for both input levels */
	struct circular_buf_ctx ctx_first, ctx_second;
	circular_buf_init(&ctx_first, temp->compaction.first_level_start,
			  temp->compaction.first_level_size,
			  get_slm_lba_info_from_addr(temp->compaction.first_level_start),
			  "COMPACT");
	circular_buf_init(&ctx_second, temp->compaction.second_level_start,
			  temp->compaction.second_level_size,
			  get_slm_lba_info_from_addr(temp->compaction.second_level_start),
			  "COMPACT");

	CSD_DEBUG_MAGIC_COMPACTION("Magic compaction init: first_slm_page=%lu, first_size=%lu, second_slm_page=%lu, second_size=%lu, output_slm_page=%lu\n",
		   get_slm_offset(ctx_first.base_addr) >> SLM_PAGE_SHIFT, ctx_first.logical_total_size,
		   get_slm_offset(ctx_second.base_addr) >> SLM_PAGE_SHIFT, ctx_second.logical_total_size,
		   get_slm_offset(temp->info.output_buf) >> SLM_PAGE_SHIFT);

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	u64 compaction_start = ktime_get_ns();
#endif
	bool adjust_block_size_flag = false; // used to determine block completion

	/* Current pointers (derived from circular buffer contexts) */
	char *first_level = circular_buf_current_ptr(&ctx_first);
	char *second_level = circular_buf_current_ptr(&ctx_second);

	/* For DecodeEntry limit parameter - use ghost page to handle wrap */
	char *first_limit = (char *)(ctx_first.base_addr + MAGIC_BUFFER_SIZE + GHOST_PAGE_SIZE);
	char *second_limit = (char *)(ctx_second.base_addr + MAGIC_BUFFER_SIZE + GHOST_PAGE_SIZE);

	size_t output_offset = 0;

	char *tmp_first;
	char *tmp_second;

	void *buf_out = (void *)temp->info.output_buf;
	void *buf_file_meta = (void *)temp->info.output_second_buf;

	size_t *file_size = (size_t *)((char *)buf_file_meta + sizeof(int));
	struct compaction_stats *stats =
		(struct compaction_stats *)((char *)buf_file_meta + sizeof(int) +
									sizeof(size_t) * MAX_OUTPUT_TABLES);
	struct compacted_file_metadata *meta =
		(struct compacted_file_metadata *)((char *)buf_file_meta + sizeof(int) +
										   sizeof(size_t) * MAX_OUTPUT_TABLES +
										   sizeof(struct compaction_stats));
	int file_size_count = 0;

	void *buf_file_out = buf_out;

	/* Initialize circular output buffer context for streaming output to host */
	struct circular_output_buf_ctx ctx_output;
	circular_output_buf_init(&ctx_output, (size_t)buf_file_out,
				get_slm_lba_info_from_addr((size_t)buf_file_out),
				"COMPACT");
	CSD_DEBUG_MAGIC_OUTPUT_CTX(ctx_output.debug_tag, "Output buffer init: slm_page=%lu\n",
		   get_slm_offset(ctx_output.base_addr) >> SLM_PAGE_SHIFT);

	size_t block_size = 0;
	char *index_block_ptr = g_index_block_ptr;
	char *index_block_append_ptr = index_block_ptr;
	struct decode_info *index_info;
	char *index_restart_ptr = g_index_restart_ptr;
	char *index_restart_append_ptr = index_restart_ptr;
	uint32_t restart_entry_count = 0;
	uint64_t *hash_entry_ptr = g_hash_entry_ptr;

	char trailer[kBlockTrailerSize];
	memset(trailer, 0, kBlockTrailerSize);

	unsigned char restart_inverval[8] = {
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
	};
	uint64_t interval_value, comp_value;
	memcpy(&interval_value, restart_inverval, sizeof(uint64_t));

	struct compaction_properties properties;
	memset(&properties, 0, sizeof(struct compaction_properties));
	uint64_t seqno;
	memset(stats, 0, sizeof(struct compaction_stats));

	NVMEV_CSD_PROFILE_REAL_START(pid, host_id);
	CSD_DEBUG("Magic Rocksdb compaction is about to begin\n");
	CSD_DEBUG("%llu %llu %llu(%llu)\n", temp->compaction.first_level_size,
			  temp->compaction.second_level_size, sstable_size,
			  datablock_threshold);

	/* Initial data availability check using circular buffer helpers */
	circular_buf_check_data(&ctx_first, SLM_PAGE_SIZE, pid, host_id);
	circular_buf_check_data(&ctx_second, SLM_PAGE_SIZE, pid, host_id);

	/* Prefetch second page for both buffers to have two consecutive pages ready */
	if (ctx_first.logical_total_size > SLM_PAGE_SIZE) {
		ctx_first.logical_offset = SLM_PAGE_SIZE;
		circular_buf_check_data(&ctx_first, SLM_PAGE_SIZE, pid, host_id);
		ctx_first.logical_offset = 0;
	}
	if (ctx_second.logical_total_size > SLM_PAGE_SIZE) {
		ctx_second.logical_offset = SLM_PAGE_SIZE;
		circular_buf_check_data(&ctx_second, SLM_PAGE_SIZE, pid, host_id);
		ctx_second.logical_offset = 0;
	}

	/* Track SLM pages for prefetching - use logical pages for circular buffers */
	size_t first_level_logical_page = 0;
	size_t second_level_logical_page = 0;
	size_t first_level_total_pages = (ctx_first.logical_total_size + SLM_PAGE_SIZE - 1) / SLM_PAGE_SIZE;
	size_t second_level_total_pages = (ctx_second.logical_total_size + SLM_PAGE_SIZE - 1) / SLM_PAGE_SIZE;

	int cumulative_first_level_block_size = 0;
	int cumulative_second_level_block_size = 0;
	int cumulative_output_block_size = 0;
	size_t output_block_offset = 0;

	CSD_DEBUG_MAGIC_COMPACTION("Compaction Start\n");

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	u64 single_start = ktime_get_ns();
#endif

	/* Cache limits and base addresses for loop to avoid struct access every iteration */
	size_t first_total_size = ctx_first.logical_total_size;
	size_t second_total_size = ctx_second.logical_total_size;

	while (ctx_first.logical_offset < first_total_size &&
	       ctx_second.logical_offset < second_total_size) {
		first_level = circular_buf_current_ptr(&ctx_first);
		second_level = circular_buf_current_ptr(&ctx_second);
		tmp_first = first_level;
		tmp_second = second_level;

		/* Notify compute progress when crossing page boundaries */
		circular_buf_notify_progress(&ctx_first);
		circular_buf_notify_progress(&ctx_second);

		/* Prefetch next page if needed */
		size_t curr_first_page = ctx_first.logical_offset >> SLM_PAGE_SHIFT;
		size_t curr_second_page = ctx_second.logical_offset >> SLM_PAGE_SHIFT; 

		if (curr_first_page != first_level_logical_page &&
		    first_level_logical_page + 1 < first_level_total_pages) {
			circular_buf_check_data_at(&ctx_first, ctx_first.logical_offset + SLM_PAGE_SIZE,
						   SLM_PAGE_SIZE, pid, host_id);
			first_level_logical_page = curr_first_page;
		}
		if (curr_second_page != second_level_logical_page &&
		    second_level_logical_page + 1 < second_level_total_pages) {
			circular_buf_check_data_at(&ctx_second, ctx_second.logical_offset + SLM_PAGE_SIZE,
						   SLM_PAGE_SIZE, pid, host_id);
			second_level_logical_page = curr_second_page;
		}

		comp_value = *(uint64_t *)first_level;
		if (comp_value == interval_value) {
			ctx_first.logical_offset += 8 + kBlockTrailerSize;
			cumulative_first_level_block_size = 0;
			continue;
		}
		comp_value = *(uint64_t *)second_level;
		if (comp_value == interval_value) {
			ctx_second.logical_offset += 8 + kBlockTrailerSize;
			cumulative_second_level_block_size = 0;
			continue;
		}

		tmp_first = DecodeEntry(tmp_first, first_limit, &info_first.shared,
								&info_first.non_shared, &info_first.value_length);
		memcpy(info_first.key, tmp_first, info_first.non_shared);
		tmp_first = tmp_first + info_first.non_shared + info_first.value_length;

		tmp_second = DecodeEntry(tmp_second, second_limit, &info_second.shared,
								 &info_second.non_shared, &info_second.value_length);
		memcpy(info_second.key, tmp_second, info_second.non_shared);
		tmp_second = tmp_second + info_second.non_shared + info_second.value_length;

		if (info_first.non_shared != info_second.non_shared) {
			NVMEV_ERROR("Key length mismatch: %d %d\n", info_first.non_shared,
						info_second.non_shared);
			BUG();
		}
		cmp_result = memcmp(info_first.key, info_second.key,
							(info_first.non_shared - kNumInternalBytes));

		/* Calculate advancement sizes */
		size_t first_advance = (tmp_first - first_level);
		size_t second_advance = (tmp_second - second_level);

		if (cmp_result == 0) {
			block_size = first_advance;
			circular_output_set_data(&ctx_output, first_level, block_size);
			index_info = &info_first;

			cumulative_first_level_block_size += first_advance;
			if (cumulative_first_level_block_size >= sst_block_size) {
				cumulative_first_level_block_size = 0;
				first_advance += 8 + kBlockTrailerSize;
			}

			cumulative_second_level_block_size += second_advance;
			if (cumulative_second_level_block_size >= sst_block_size) {
				cumulative_second_level_block_size = 0;
				second_advance += 8 + kBlockTrailerSize;
			}

			/* Update logical offsets in circular buffer contexts */
			ctx_first.logical_offset += first_advance;
			ctx_second.logical_offset += second_advance;

			stats->num_input_entries += 2;
			stats->raw_input_key_size += (info_first.shared + info_first.non_shared);
			stats->raw_input_key_size += (info_second.shared + info_second.non_shared);
			stats->raw_input_value_size +=
				(info_first.value_length + info_second.value_length);
		} else if (cmp_result < 0) {
			block_size = first_advance;
			circular_output_set_data(&ctx_output, first_level, block_size);
			index_info = &info_first;

			cumulative_first_level_block_size += first_advance;
			if (cumulative_first_level_block_size >= sst_block_size) {
				cumulative_first_level_block_size = 0;
				first_advance += 8 + kBlockTrailerSize;
			}

			/* Update logical offset in circular buffer context */
			ctx_first.logical_offset += first_advance;

			stats->num_input_entries++;
			stats->raw_input_key_size += (info_first.shared + info_first.non_shared);
			stats->raw_input_value_size += info_first.value_length;
		} else {
			block_size = second_advance;
			circular_output_set_data(&ctx_output, second_level, block_size);
			index_info = &info_second;

			cumulative_second_level_block_size += second_advance;
			if (cumulative_second_level_block_size >= sst_block_size) {
				cumulative_second_level_block_size = 0;
				second_advance += 8 + kBlockTrailerSize;
			}

			/* Update logical offset in circular buffer context */
			ctx_second.logical_offset += second_advance;

			stats->num_input_entries++;
			stats->raw_input_key_size += (info_second.shared + info_second.non_shared);
			stats->raw_input_value_size += info_second.value_length;
		}
		stats->num_output_entries++;

		if (bits_per_key > 0 && bloom_flag) {
			hash_entry_ptr[properties.num_entries] =
				XXPH3_64bits(index_info->key, index_info->non_shared - kNumInternalBytes);
		}

		seqno =
			DecodeFixed64(index_info->key + index_info->non_shared - kNumInternalBytes) >>
			8;
		if (output_offset == 0) {
			memcpy(meta[file_size_count].smallest_key, index_info->key,
				   index_info->non_shared);
			meta[file_size_count].smallest_klen = index_info->non_shared;
			meta[file_size_count].smallest_seqno = seqno;
			meta[file_size_count].largest_seqno = seqno;
		}
		if (seqno < meta[file_size_count].smallest_seqno) {
			meta[file_size_count].smallest_seqno = seqno;
		}
		if (seqno > meta[file_size_count].largest_seqno) {
			meta[file_size_count].largest_seqno = seqno;
		}

		if (adjust_block_size_flag == false && (block_size < 1024)) {
			sst_block_size -= block_size;
			adjust_block_size_flag = true;
		}
		cumulative_output_block_size += block_size;
		if (cumulative_output_block_size >= sst_block_size) {
			/* Write restart interval */
			circular_output_set_data(&ctx_output, restart_inverval, 8);
			cumulative_output_block_size += 8;

			__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
										 &index_restart_append_ptr, &restart_entry_count,
										 output_block_offset,
										 (index_block_append_ptr - index_block_ptr),
										 cumulative_output_block_size);

			/* CRC calculation - get pointer to block start in circular buffer.
			 * Ghost page ensures data continuity even if block wraps around. */
			if (crc_flag) {
				char *block_ptr = circular_output_buf_ptr(&ctx_output,
					ctx_output.logical_write_offset - cumulative_output_block_size);
				WARN_GHOST_OVERRUN(cumulative_output_block_size);
				set_xxh3_to_block(block_ptr, cumulative_output_block_size,
								  output_block_offset, trailer);
			}
			/* Write trailer */
			circular_output_set_data(&ctx_output, trailer, kBlockTrailerSize);
			output_offset += block_size;
			output_offset += 8 + kBlockTrailerSize;

			output_block_offset = output_offset;
			cumulative_output_block_size = 0;
			__update_compaction_properties(&properties, index_info, true);
		} else {
			output_offset += block_size;
			__update_compaction_properties(&properties, index_info, false);
		}

		if (output_offset >= datablock_threshold) {
			CSD_DEBUG_MAGIC_COMPACTION("Finalize data block at output_offset=%lu\n", output_offset);
			if (cumulative_output_block_size > 0) {
				/* Write restart interval */
				circular_output_set_data(&ctx_output, restart_inverval, 8);
				cumulative_output_block_size += 8;
				__update_compaction_properties(&properties, NULL, true);

				__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
											 &index_restart_append_ptr,
											 &restart_entry_count, output_block_offset,
											 (index_block_append_ptr - index_block_ptr),
											 cumulative_output_block_size);

				/* CRC calculation - ghost page ensures data continuity */
				if (crc_flag) {
					char *block_ptr = circular_output_buf_ptr(&ctx_output,
						ctx_output.logical_write_offset - cumulative_output_block_size);
					WARN_GHOST_OVERRUN(cumulative_output_block_size);
					set_xxh3_to_block(block_ptr, cumulative_output_block_size,
									  output_block_offset, trailer);
				}
				/* Write trailer */
				circular_output_set_data(&ctx_output, trailer, kBlockTrailerSize);
				output_offset += 8 + kBlockTrailerSize;

				output_block_offset = output_offset;
				cumulative_output_block_size = 0;
			}

			memcpy(meta[file_size_count].largest_key, index_info->key,
				   index_info->non_shared);
			meta[file_size_count].largest_klen = index_info->non_shared;
			properties.data_size = output_offset;
			properties.tail_start_offset = output_offset;

			index_restart_append_ptr =
				PutFixed32(index_restart_append_ptr, restart_entry_count);
			CSD_DEBUG_MAGIC_COMPACTION("data block size: %lu\n", output_offset);
			CSD_DEBUG_MAGIC_COMPACTION("index block size: %lu\n",
					  index_block_append_ptr - index_block_ptr);
			CSD_DEBUG_MAGIC_COMPACTION("index restart size: %lu (entry: %d)\n",
					  index_restart_append_ptr - index_restart_ptr, restart_entry_count);

			/* Use magic finalize function for circular output buffer */
			output_offset = __magic_rocksdb_finalized_sstable(
				&ctx_output, index_block_ptr, index_block_append_ptr,
				index_restart_ptr, index_restart_append_ptr, restart_entry_count,
				bits_per_key, bloom_flag, hash_entry_ptr,
				&properties, &(temp->compaction.host_properties));

			/* Notify remaining bytes up to sstable_size as ready (no actual write needed).
			 * This matches original compaction behavior and allows host to read the full SST. */
			circular_output_notify_range(&ctx_output, sstable_size - output_offset);

			meta[file_size_count].tail_size =
				output_offset - properties.tail_start_offset;

			memcpy(&(meta[file_size_count].properties), &properties,
				   sizeof(struct compaction_properties));
			memset(&properties, 0, sizeof(struct compaction_properties));
			file_size[file_size_count++] = output_offset;
#if (_MAGIC_TIME_BREAKDOWN_ == 1)
			{
				u64 single_end = ktime_get_ns();
				u64 elapsed_us = (single_end - single_start) / 1000;
				single_start = ktime_get_ns();
				printk(KERN_INFO "[COMPACTION] single time_us=%llu\n", elapsed_us);
			}
#endif

			/* For circular buffer, don't change buf_file_out - we continue writing
			 * sequentially. Reset output_offset for next SSTable's local tracking. */
			output_offset = 0;
			output_block_offset = 0;
			cumulative_output_block_size = 0;
			memset(index_block_ptr, 0, 1024 * 1024);
			index_block_append_ptr = index_block_ptr;
			memset(index_restart_ptr, 0, 1024 * 1024);
			index_restart_append_ptr = index_restart_ptr;
			memset(hash_entry_ptr, 0, 128 * 1024);
			restart_entry_count = 0;
			CSD_DEBUG_MAGIC_COMPACTION("Finalize data block done\n");
		}
	}

	/* Notify final progress for both levels */
	circular_buf_notify_progress(&ctx_first);
	circular_buf_notify_progress(&ctx_second);

	/* Determine which level has remaining data and use a unified context for the remainder */
	struct circular_buf_ctx *ctx_remain;
	char *remain_limit;
	size_t remain_total_size = 0;
	size_t remain_logical_page;
	size_t remain_total_pages;
	if (!circular_buf_at_end(&ctx_second)) {
		/* Second level has remaining data, use it */
		ctx_remain = &ctx_second;
		remain_limit = second_limit;
		cumulative_first_level_block_size = cumulative_second_level_block_size;
		remain_total_size = second_total_size;
		remain_logical_page = second_level_logical_page;
		remain_total_pages = second_level_total_pages;
	} else {
		/* First level has remaining data (or both are done) */
		ctx_remain = &ctx_first;
		remain_limit = first_limit;
		remain_total_size = first_total_size;
		remain_logical_page = first_level_logical_page;
		remain_total_pages = first_level_total_pages;
	}
	circular_buf_notify_progress(ctx_remain);

	while (ctx_remain->logical_offset < remain_total_size) {
		first_level = circular_buf_current_ptr(ctx_remain);
		tmp_first = first_level;

		/* Ensure current page data is ready (critical for wrap-around) */
		circular_buf_check_data(ctx_remain, SLM_PAGE_SIZE, pid, host_id);

		/* Notify compute progress */
		circular_buf_notify_progress(ctx_remain);

		/* Prefetch next page if needed */
		size_t curr_remain_page = ctx_remain->logical_offset >> SLM_PAGE_SHIFT;
		if (curr_remain_page != remain_logical_page &&
		    remain_logical_page + 1 < remain_total_pages) {
			circular_buf_check_data_at(ctx_remain, ctx_remain->logical_offset + SLM_PAGE_SIZE,
						   SLM_PAGE_SIZE, pid, host_id);
			remain_logical_page = curr_remain_page;
		}

		if (circular_buf_remaining(ctx_remain) == (8 + kBlockTrailerSize)) {
			ctx_remain->logical_offset = ctx_remain->logical_total_size;
			break;
		}

		tmp_first = DecodeEntry(tmp_first, remain_limit, &info_first.shared,
								&info_first.non_shared, &info_first.value_length);
		if (tmp_first == NULL) {
			NVMEV_ERROR("DecodeEntry failed in remainder loop at logical_offset=%zu, page=%zu\n",
						ctx_remain->logical_offset, ctx_remain->logical_offset / SLM_PAGE_SIZE);
			BUG();
		}
		memcpy(info_first.key, tmp_first, info_first.non_shared);
		tmp_first = tmp_first + info_first.non_shared + info_first.value_length;

		size_t advance = (tmp_first - first_level);
		block_size = advance;
		circular_output_set_data(&ctx_output, first_level, block_size);
		index_info = &info_first;

		cumulative_first_level_block_size += advance;
		if (cumulative_first_level_block_size >= sst_block_size) {
			cumulative_first_level_block_size = 0;
			advance += 8 + kBlockTrailerSize;
		}

		ctx_remain->logical_offset += advance;

		stats->num_input_entries++;
		stats->raw_input_key_size += (index_info->shared + index_info->non_shared);
		stats->raw_input_value_size += index_info->value_length;
		stats->num_output_entries++;

		if (bits_per_key > 0 && bloom_flag) {
			hash_entry_ptr[properties.num_entries] =
				XXPH3_64bits(index_info->key, index_info->non_shared - kNumInternalBytes);
		}

		seqno =
			DecodeFixed64(index_info->key + index_info->non_shared - kNumInternalBytes) >>
			8;
		if (output_offset == 0) {
			memcpy(meta[file_size_count].smallest_key, index_info->key,
				   index_info->non_shared);
			meta[file_size_count].smallest_klen = index_info->non_shared;
			meta[file_size_count].smallest_seqno = seqno;
			meta[file_size_count].largest_seqno = seqno;
		}
		if (seqno < meta[file_size_count].smallest_seqno) {
			meta[file_size_count].smallest_seqno = seqno;
		}
		if (seqno > meta[file_size_count].largest_seqno) {
			meta[file_size_count].largest_seqno = seqno;
		}

		cumulative_output_block_size += block_size;
		if (cumulative_output_block_size >= sst_block_size) {
			/* Write restart interval */
			circular_output_set_data(&ctx_output, restart_inverval, 8);
			cumulative_output_block_size += 8;

			__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
										 &index_restart_append_ptr, &restart_entry_count,
										 output_block_offset,
										 (index_block_append_ptr - index_block_ptr),
										 cumulative_output_block_size);

			/* CRC calculation - ghost page ensures data continuity */
			if (crc_flag) {
				char *block_ptr = circular_output_buf_ptr(&ctx_output,
					ctx_output.logical_write_offset - cumulative_output_block_size);
				WARN_GHOST_OVERRUN(cumulative_output_block_size);
				set_xxh3_to_block(block_ptr, cumulative_output_block_size,
								  output_block_offset, trailer);
			}
			/* Write trailer */
			circular_output_set_data(&ctx_output, trailer, kBlockTrailerSize);
			output_offset += block_size;
			output_offset += 8 + kBlockTrailerSize;

			output_block_offset = output_offset;
			cumulative_output_block_size = 0;
			__update_compaction_properties(&properties, index_info, true);
		} else {
			output_offset += block_size;
			__update_compaction_properties(&properties, index_info, false);
		}

		if (output_offset >= datablock_threshold) {
			if (cumulative_output_block_size > 0) {
				/* Write restart interval */
				circular_output_set_data(&ctx_output, restart_inverval, 8);
				cumulative_output_block_size += 8;
				__update_compaction_properties(&properties, NULL, true);

				__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
											 &index_restart_append_ptr,
											 &restart_entry_count, output_block_offset,
											 (index_block_append_ptr - index_block_ptr),
											 cumulative_output_block_size);

				/* CRC calculation - ghost page ensures data continuity */
				if (crc_flag) {
					char *block_ptr = circular_output_buf_ptr(&ctx_output,
						ctx_output.logical_write_offset - cumulative_output_block_size);
					WARN_GHOST_OVERRUN(cumulative_output_block_size);
					set_xxh3_to_block(block_ptr, cumulative_output_block_size,
									  output_block_offset, trailer);
				}
				/* Write trailer */
				circular_output_set_data(&ctx_output, trailer, kBlockTrailerSize);
				output_offset += 8 + kBlockTrailerSize;

				output_block_offset = output_offset;
				cumulative_output_block_size = 0;
			}

			memcpy(meta[file_size_count].largest_key, index_info->key,
				   index_info->non_shared);
			meta[file_size_count].largest_klen = index_info->non_shared;
			properties.data_size = output_offset;
			properties.tail_start_offset = output_offset;

			index_restart_append_ptr =
				PutFixed32(index_restart_append_ptr, restart_entry_count);
			CSD_DEBUG_MAGIC_COMPACTION("data block size: %lu\n", output_offset);
			CSD_DEBUG_MAGIC_COMPACTION("index block size: %lu\n",
					  index_block_append_ptr - index_block_ptr);
			CSD_DEBUG_MAGIC_COMPACTION("index restart size: %lu (entry: %d)\n",
					  index_restart_append_ptr - index_restart_ptr, restart_entry_count);

			/* Use magic finalize function for circular output buffer */
			output_offset = __magic_rocksdb_finalized_sstable(
				&ctx_output, index_block_ptr, index_block_append_ptr,
				index_restart_ptr, index_restart_append_ptr, restart_entry_count,
				bits_per_key, bloom_flag, hash_entry_ptr,
				&properties, &(temp->compaction.host_properties));

			/* Notify remaining bytes up to sstable_size as ready (no actual write needed).
			 * This matches original compaction behavior and allows host to read the full SST. */
			circular_output_notify_range(&ctx_output, sstable_size - output_offset);

			meta[file_size_count].tail_size =
				output_offset - properties.tail_start_offset;

			memcpy(&(meta[file_size_count].properties), &properties,
				   sizeof(struct compaction_properties));
			memset(&properties, 0, sizeof(struct compaction_properties));
			file_size[file_size_count++] = output_offset;
#if (_MAGIC_TIME_BREAKDOWN_ == 1)
		{
			u64 single_end = ktime_get_ns();
			u64 elapsed_us = (single_end - single_start) / 1000;
			single_start = ktime_get_ns();
			printk(KERN_INFO "[COMPACTION] single time_us=%llu\n", elapsed_us);
		}
#endif

			/* For circular buffer, don't change buf_file_out - continue writing sequentially */
			output_offset = 0;
			output_block_offset = 0;
			cumulative_output_block_size = 0;
			memset(index_block_ptr, 0, 1024 * 1024);
			index_block_append_ptr = index_block_ptr;
			memset(index_restart_ptr, 0, 1024 * 1024);
			index_restart_append_ptr = index_restart_ptr;
			memset(hash_entry_ptr, 0, 128 * 1024);
			restart_entry_count = 0;
		}
	}

	/* Final progress notification after remaining loop */
	circular_buf_notify_progress(ctx_remain);

	if (output_offset > 0) {
		if (cumulative_output_block_size > 0) {
			/* Write restart interval */
			circular_output_set_data(&ctx_output, restart_inverval, 8);
			cumulative_output_block_size += 8;
			__update_compaction_properties(&properties, NULL, true);

			__rocksdb_handle_index_block(index_info, &index_block_append_ptr,
										 &index_restart_append_ptr, &restart_entry_count,
										 output_block_offset,
										 (index_block_append_ptr - index_block_ptr),
										 cumulative_output_block_size);

			/* CRC calculation - ghost page ensures data continuity */
			if (crc_flag) {
				char *block_ptr = circular_output_buf_ptr(&ctx_output,
					ctx_output.logical_write_offset - cumulative_output_block_size);
				WARN_GHOST_OVERRUN(cumulative_output_block_size);
				set_xxh3_to_block(block_ptr, cumulative_output_block_size,
								  output_block_offset, trailer);
			}
			/* Write trailer */
			circular_output_set_data(&ctx_output, trailer, kBlockTrailerSize);
			output_offset += 8 + kBlockTrailerSize;

			output_block_offset = output_offset;
			cumulative_output_block_size = 0;
		}

		memcpy(meta[file_size_count].largest_key, index_info->key,
			   index_info->non_shared);
		meta[file_size_count].largest_klen = index_info->non_shared;
		properties.data_size = output_offset;
		properties.tail_start_offset = output_offset;

		index_restart_append_ptr =
			PutFixed32(index_restart_append_ptr, restart_entry_count);
		CSD_DEBUG_MAGIC_COMPACTION("data block size: %lu\n", output_offset);
		CSD_DEBUG_MAGIC_COMPACTION("index block size: %lu\n", index_block_append_ptr - index_block_ptr);
		CSD_DEBUG_MAGIC_COMPACTION("index restart size: %lu (entry: %d)\n",
				  index_restart_append_ptr - index_restart_ptr, restart_entry_count);
	
#if (_MAGIC_TIME_BREAKDOWN_ == 1)
		{
			u64 single_end = ktime_get_ns();
			u64 elapsed_us = (single_end - single_start) / 1000;
			printk(KERN_INFO "[COMPACTION] single time_us=%llu\n", elapsed_us);
			single_start = ktime_get_ns();
		}
#endif

		/* Use magic finalize function for circular output buffer */
		output_offset = __magic_rocksdb_finalized_sstable(
			&ctx_output, index_block_ptr, index_block_append_ptr,
			index_restart_ptr, index_restart_append_ptr, restart_entry_count,
			bits_per_key, bloom_flag, hash_entry_ptr,
			&properties, &(temp->compaction.host_properties));

		/* Notify remaining bytes up to sstable_size as ready (no actual write needed).
		 * This matches original compaction behavior and allows host to read the full SST. */
		// circular_output_notify_range(&ctx_output, sstable_size - output_offset);

		meta[file_size_count].tail_size = output_offset - properties.tail_start_offset;

		memcpy(&(meta[file_size_count].properties), &properties,
			   sizeof(struct compaction_properties));
		file_size[file_size_count++] = output_offset;
	}

	/* Finalize circular output buffer - set total size for host */
	circular_output_finalize(&ctx_output);

	memcpy(buf_file_meta, &file_size_count, sizeof(int));

	/* Debug: dump metadata key/seqno fields before marking ready */
	// printk(KERN_INFO "[META] file_size_count=%d\n", file_size_count);
	// for (int i = 0; i < file_size_count; i++) {
	// 	printk(KERN_INFO "[META] file[%d]: smallest_key=%.*s (len=%u), largest_key=%.*s (len=%u), "
	// 		   "smallest_seqno=%llu, largest_seqno=%llu\n",
	// 		   i,
	// 		   meta[i].smallest_klen, meta[i].smallest_key,
	// 		   meta[i].smallest_klen,
	// 		   meta[i].largest_klen, meta[i].largest_key,
	// 		   meta[i].largest_klen,
	// 		   meta[i].smallest_seqno, meta[i].largest_seqno);
	// }

	notify_if_slm_data_ready((size_t)buf_file_meta, FRONT_METADATA_SIZE);

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	{
		u64 compaction_end = ktime_get_ns();
		u64 elapsed_us = (compaction_end - compaction_start) / 1000;
		printk(KERN_INFO "[COMPACTION] time_us=%llu\n", elapsed_us);
	}
#endif

	NVMEV_CSD_PROFILE_REAL_END(pid, host_id);

	/* Summary log for circular buffer compaction */
	CSD_DEBUG_MAGIC_COMPACTION("Compaction DONE: first_final_off=%lu/%lu, second_final_off=%lu/%lu\n",
		   ctx_first.logical_offset, ctx_first.logical_total_size,
		   ctx_second.logical_offset, ctx_second.logical_total_size);
	CSD_DEBUG_MAGIC_COMPACTION("Output: files=%d, entries=%llu, input_entries=%llu\n",
		   file_size_count, stats->num_output_entries, stats->num_input_entries);

	for (int i = 0; i < file_size_count; i++) {
		CSD_DEBUG_MAGIC_COMPACTION("%d - %llu\n", i, file_size[i]);
	}

	size_t total_io_size = 0;
	if (output_offset > 0) {
		total_io_size =
			((file_size_count - 1) * sstable_size) + file_size[file_size_count - 1];
	} else {
		total_io_size = ((file_size_count)*sstable_size);
	}

#if (SUPPORT_ASYNC_COMPUTE == 1)
	total_io_size += FRONT_METADATA_SIZE;
#endif

	return total_io_size;
}

size_t __rocksdb_crc_calculation(void *buf_in, void *buf_out, size_t size, void *param)
{
#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	u64 crc_start_ns = ktime_get_ns();
#endif

	struct CSD_PARAMS *temp = (struct CSD_PARAMS *)param;
	int pid = temp->profile_info.pid;
	int host_id = temp->profile_info.host_id;
	size_t sstable_size = temp->rocksdb_crc_params.sstable_size;
	size_t datablock_threshold = temp->rocksdb_crc_params.datablock_threshold;
	int num_cores = temp->rocksdb_crc_params.num_cores;
	int core_id = temp->rocksdb_crc_params.core_id;
	size_t output_offset = 0;
	struct decode_info entry_info;
	size_t entry_size;

	bool decode_first_entry = true;

	/* For tracking cumulative block sizes (multiple entries per block) */
	int cumulative_output_block_size = 0;
	size_t output_block_offset = 0;
	char *input_block_start;

	/* Restart interval marker: {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00} */
	char restart_interval[8] = {
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
	};
	uint64_t interval_value;
	memcpy(&interval_value, restart_interval, sizeof(uint64_t));

	NVMEV_CSD_PROFILE_REAL_START(pid, host_id);

	char *input_start = (char *)buf_in + FRONT_METADATA_SIZE;
	char *output_start = (char *)buf_out + FRONT_METADATA_SIZE;
	char *input_ptr = input_start;
	char *output_ptr = output_start;
	char *input_limit = input_ptr + (size - FRONT_METADATA_SIZE);
	char *tmp_input_ptr;
	char *last_ptr;

	char trailer[kBlockTrailerSize];
	memset(trailer, 0, kBlockTrailerSize);

	check_data_using_ptr((size_t)input_ptr, SLM_PAGE_SIZE, pid, host_id);

	size_t input_ptr_slm_page_num =
		get_slm_offset((size_t)input_ptr) / SLM_PAGE_SIZE; // SLM loaded page number
	size_t last_input_ptr_slm_page_num = get_slm_offset((size_t)input_limit) / SLM_PAGE_SIZE;

	CSD_DEBUG("input_ptr_slm_page: %llu, last_input_ptr_slm_page: %llu\n",
			  input_ptr_slm_page_num, last_input_ptr_slm_page_num);

	input_block_start = input_ptr;

	while (input_ptr < input_limit) {
		tmp_input_ptr = input_ptr;

		if ((get_slm_offset((size_t)input_ptr) / SLM_PAGE_SIZE) + 1 != input_ptr_slm_page_num &&
			input_ptr_slm_page_num + 1 < last_input_ptr_slm_page_num) {
			if (check_data_using_ptr((size_t)input_ptr + SLM_PAGE_SIZE, SLM_PAGE_SIZE,
									 pid, host_id) == 0) {
				last_input_ptr_slm_page_num = input_ptr_slm_page_num; // current page is the last page

				/* Get final page leftovers and set the input_limit */
				size_t leftover = get_slm_last_page_leftover((size_t)input_ptr);

				input_limit =
					(char *)get_slm_addr(last_input_ptr_slm_page_num * SLM_PAGE_SIZE) + leftover;
				CSD_DEBUG("[core%d] finalized input total size is: %llu (%llu, %llu)",
						  core_id, (input_limit - input_start), input_limit, input_start);

				if (!(input_ptr < input_limit)) {
					break;
				}
			}
			// CSD_DEBUG("checked for page %llu\n", input_ptr_slm_page_num);
			input_ptr_slm_page_num++;
		}

		if (decode_first_entry == true) {
			tmp_input_ptr = DecodeEntry(tmp_input_ptr, input_limit, &entry_info.shared,
										&entry_info.non_shared, &entry_info.value_length);
			if (!(entry_info.shared == 0 && entry_info.non_shared == 24)) {
				// This means that datablock has finished
				CSD_DEBUG(
					"[core%d] Wrong decode result. This is the end datablock: output_offset: %llu, input_offset: %llu\n",
					core_id, output_offset, (input_ptr - input_start));
				break;
			}

			/* Calculate entry size (varint header + key + value) */
			tmp_input_ptr = tmp_input_ptr + entry_info.non_shared + entry_info.value_length;
			entry_size = (tmp_input_ptr - input_ptr);

			decode_first_entry = false;
		} else {
			tmp_input_ptr = input_ptr + entry_size;
		}
		cumulative_output_block_size += entry_size;

		/* Check if next 8 bytes are restart entry (block boundary) */
		uint64_t next_value = *(uint64_t *)tmp_input_ptr;
		if (next_value == interval_value) {
			/* End of input block - skip restart entry and trailer in input */
			tmp_input_ptr = tmp_input_ptr + 8 + kBlockTrailerSize;

			/* Calculate CRC and write output block */
			cumulative_output_block_size += 8; // add restart size

			if ((output_block_offset / SLM_PAGE_SIZE) % num_cores == core_id) {
				set_xxh3_to_block(input_block_start, cumulative_output_block_size,
								  output_block_offset, trailer);
				set_data_from_ptr((size_t)output_ptr + output_block_offset,
								  (size_t)input_block_start, cumulative_output_block_size);
				set_data_from_ptr((size_t)output_ptr + output_block_offset +
									  cumulative_output_block_size,
								  (size_t)trailer, kBlockTrailerSize);
			} else {
				/* Copy input block data */
				set_data_from_ptr((size_t)output_ptr + output_block_offset, 
								  (size_t)input_block_start,
								  cumulative_output_block_size + kBlockTrailerSize);
			}

			output_offset =
					output_block_offset + cumulative_output_block_size + kBlockTrailerSize;
			output_block_offset = output_offset;
			cumulative_output_block_size = 0;
			input_block_start = tmp_input_ptr;
			decode_first_entry = true; // next entry is the first entry of new block
		}

		input_ptr = tmp_input_ptr;

		if (output_offset >= datablock_threshold) {
			/* copy metaindex, index, footer */
			check_data_using_ptr((size_t)input_ptr, (sstable_size - output_offset),
								 pid, host_id);
			set_data_from_ptr((size_t)output_ptr + output_offset, (size_t)input_ptr,
							  (sstable_size - output_offset));
			CSD_DEBUG("[core%d] copy meta at %lu, %lu\n", core_id, output_offset,
					  (sstable_size - output_offset));

			input_ptr += (sstable_size - output_offset);
			output_ptr += sstable_size;
			CSD_DEBUG("[core%d] output_offset: %lu, input_offset: %lu\n", core_id,
					  output_offset, (input_ptr - input_start));

			output_offset = 0;
			output_block_offset = 0;
			cumulative_output_block_size = 0;
			input_block_start = input_ptr;
			input_ptr_slm_page_num =
				get_slm_offset((size_t)input_ptr) / SLM_PAGE_SIZE; // update slm page num
			CSD_DEBUG("input ptr is now %llu\n", input_ptr_slm_page_num);
		}
	}

	CSD_DEBUG("[core%d] break from while loop %llu %llu\n", core_id, output_offset,
			  (input_ptr - input_start));

	/* Get the leftover size by finding the final page */
	last_ptr = input_ptr;
	while (last_ptr < input_limit) {
		if (check_data_using_ptr((size_t)last_ptr + SLM_PAGE_SIZE, SLM_PAGE_SIZE, pid,
								 host_id) == 0) {
			last_input_ptr_slm_page_num =
				get_slm_offset((size_t)last_ptr) / SLM_PAGE_SIZE; // current page is the last page

			/* Get final page leftovers and set the input_limit */
			size_t leftover = get_slm_last_page_leftover((size_t)last_ptr);

			input_limit =
				(char *)get_slm_addr(last_input_ptr_slm_page_num * SLM_PAGE_SIZE) + leftover;
			CSD_DEBUG("[core%d] finalized input total size is: %llu (%llu, %llu)",
					  core_id, (input_limit - input_start), input_limit, input_start);
			break;
		}
		last_ptr += SLM_PAGE_SIZE;
	}

	if ((input_ptr < input_limit)) {
		CSD_DEBUG("[core%d] copy leftovers %llu\n", core_id, (input_limit - input_ptr));
		check_data_using_ptr((size_t)input_ptr, (input_limit - input_ptr), pid, host_id);
		set_data_from_ptr((size_t)output_ptr + output_offset, (size_t)input_ptr,
						  (input_limit - input_ptr));
	}

	/* Copy FRONT_METADATA_SIZE */
	set_data_from_ptr((size_t)buf_out, (size_t)buf_in, FRONT_METADATA_SIZE);

	NVMEV_CSD_PROFILE_REAL_END(pid, host_id);
#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	{
		u64 crc_end_ns = ktime_get_ns();
		size_t output_bytes = (input_limit - (char *)buf_in);
		u64 elapsed_us = (crc_end_ns - crc_start_ns) / 1000;
		printk(KERN_INFO "[LINEAR_CRC] core_id=%d time_us=%llu bytes=%lu us_per_MB=%llu\n",
		       core_id, elapsed_us, output_bytes,
		       elapsed_us * 1024 * 1024 / (output_bytes + 1));
	}
#endif

	return (input_limit - (char *)buf_in);
}

/**
 * __magic_rocksdb_crc_calculation - Calculate CRC for magic compaction output
 * @param: Pointer to rocksdb_magic_crc_params structure
 *
 * Reads from intermediate circular buffer (compaction output without CRC),
 * calculates CRC for each data block, and writes to final output buffer.
 * Uses circular buffer contexts for streaming producer-consumer operation.
 *
 * The compaction task writes data blocks to intermediate buffer WITH trailers
 * but the trailer CRC field is zeroed. This function:
 * 1. Reads data from intermediate buffer as it becomes available
 * 2. Detects block boundaries (restart interval markers)
 * 3. Calculates XXH3 CRC for each complete block
 * 4. Writes data + proper CRC trailer to final output buffer
 *
 * Returns: Total bytes written to output buffer
 */
size_t __magic_rocksdb_crc_calculation(void *param)
{
	struct rocksdb_magic_crc_params *crc_params = (struct rocksdb_magic_crc_params *)param;
	int pid = 0;
	int host_id = 0;
	size_t sstable_size = crc_params->sstable_size;
	size_t datablock_threshold = crc_params->datablock_threshold;
	int num_cores = crc_params->num_cores;
	int core_id = crc_params->core_id;

	/* Per-core debug tag so messages are distinguishable (e.g., "CRC0", "CRC1") */
	char crc_tag[8];
	snprintf(crc_tag, sizeof(crc_tag), "CRC%d", core_id);

	/* Initialize circular buffer context for input (compaction output) */
	struct circular_buf_ctx ctx_input;
	circular_buf_init(&ctx_input, crc_params->input_buf,
			  crc_params->input_logical_size,
			  get_slm_lba_info_from_addr(crc_params->input_buf),
			  crc_tag);

	/* Initialize circular output buffer context for final output */
	struct circular_output_buf_ctx ctx_output;
	circular_output_buf_init(&ctx_output, crc_params->output_buf,
				 get_slm_lba_info_from_addr(crc_params->output_buf),
				 crc_tag);

	struct decode_info entry_info;
	size_t entry_size;
	bool decode_first_entry = true;

	/* For tracking cumulative block sizes */
	int cumulative_output_block_size = 0;
	size_t output_block_offset = 0;
	size_t output_offset = 0;
	char *input_block_start;
	size_t input_block_start_logical_offset = 0;

	/* Block counter for interleaved CRC processing */
	int block_num = 0;

	/* Restart interval marker: {0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00} */
	char restart_interval[8] = {
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
	};
	uint64_t interval_value;
	memcpy(&interval_value, restart_interval, sizeof(uint64_t));

	char trailer[kBlockTrailerSize];
	memset(trailer, 0, kBlockTrailerSize);

	/* For DecodeEntry limit parameter - use ghost page to handle wrap */
	char *input_limit = (char *)(ctx_input.base_addr + MAGIC_BUFFER_SIZE + GHOST_PAGE_SIZE);

	NVMEV_CSD_PROFILE_REAL_START(pid, host_id);

	CSD_DEBUG_MAGIC_CRC("Magic CRC: input_slm_page=%lu, output_slm_page=%lu, input_size=%lu, num_cores=%d, core_id=%d\n",
		   get_slm_offset(crc_params->input_buf) >> SLM_PAGE_SHIFT,
		   get_slm_offset(crc_params->output_buf) >> SLM_PAGE_SHIFT,
		   crc_params->input_logical_size, num_cores, core_id);

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	u64 crc_start_ns = ktime_get_ns();
#endif
	/* Wait for initial data from compaction */
	circular_buf_check_data(&ctx_input, SLM_PAGE_SIZE, pid, host_id);

	/* Initialize input_block_start before the loop */
	input_block_start = circular_buf_current_ptr(&ctx_input);
	input_block_start_logical_offset = ctx_input.logical_offset;

	while (!circular_buf_at_end(&ctx_input)) {
		char *input_ptr = circular_buf_current_ptr(&ctx_input);
		char *tmp_input_ptr = input_ptr;

		/* Notify progress and ensure data availability */
		circular_buf_notify_progress(&ctx_input);
		circular_buf_check_data_at(&ctx_input, ctx_input.logical_offset + SLM_PAGE_SIZE,
						   SLM_PAGE_SIZE, pid, host_id);

		/* Decode first entry of block to get entry structure */
		if (decode_first_entry) {
			tmp_input_ptr = DecodeEntry(tmp_input_ptr, input_limit, &entry_info.shared,
						    &entry_info.non_shared, &entry_info.value_length);

			/* Validate decoded entry - same check as original CRC code.
			 * First entry of each block must have shared=0 and non_shared=24 (key size).
			 * If this check fails, we've hit metadata instead of data blocks. */
			if (!(entry_info.shared == 0 && entry_info.non_shared == 24)) {
				CSD_DEBUG_MAGIC_CRC("Magic CRC[%d]: End of data blocks detected (shared=%u, non_shared=%u), output_offset=%lu\n",
						    core_id, entry_info.shared, entry_info.non_shared, output_offset);
				break;
			}

			/* Calculate entry size (varint header + key + value) */
			tmp_input_ptr = tmp_input_ptr + entry_info.non_shared + entry_info.value_length;
			entry_size = (tmp_input_ptr - input_ptr);
			decode_first_entry = false;
		} else {
			tmp_input_ptr = input_ptr + entry_size;
		}
		cumulative_output_block_size += entry_size;

		/* Check if next 8 bytes are restart interval (block boundary) */
		uint64_t next_value = *(uint64_t *)tmp_input_ptr;
		if (next_value == interval_value) {
			/* End of input block - skip restart entry and trailer in input */
			tmp_input_ptr = tmp_input_ptr + 8 + kBlockTrailerSize;

			/* Add restart size to cumulative block size */
			cumulative_output_block_size += 8;

			/* Both branches read block (+trailer) contiguously from the input
			 * ring, relying on the ghost mirror when the block straddles the wrap */
			WARN_GHOST_OVERRUN(cumulative_output_block_size + kBlockTrailerSize);

			/* Interleaved CRC: only compute CRC if this block belongs to this core */
			if (block_num % num_cores == core_id) {
				/* This core handles this block - calculate CRC */
				set_xxh3_to_block(input_block_start, cumulative_output_block_size,
						  output_block_offset, trailer);
				circular_output_set_data(&ctx_output, input_block_start, cumulative_output_block_size);
				circular_output_set_data(&ctx_output, trailer, kBlockTrailerSize);
			} else {
				/* Other core handles this block - pass through with existing trailer */
				circular_output_set_data(&ctx_output, input_block_start, cumulative_output_block_size + kBlockTrailerSize);
			}

			output_offset = output_block_offset + cumulative_output_block_size + kBlockTrailerSize;
			output_block_offset = output_offset;
			cumulative_output_block_size = 0;
			input_block_start = tmp_input_ptr;
			input_block_start_logical_offset = ctx_input.logical_offset + (tmp_input_ptr - input_ptr);
			decode_first_entry = true;
			block_num++;
		}

		ctx_input.logical_offset += (tmp_input_ptr - input_ptr);
		input_ptr = tmp_input_ptr;

		if (output_offset >= datablock_threshold) {
			/* Copy metadata section (filter, index, properties, footer) */
			size_t meta_size = sstable_size - output_offset;
			circular_buf_check_data(&ctx_input, meta_size, pid, host_id);

			circular_output_set_data_from_circular(&ctx_output, &ctx_input,
					  ctx_input.logical_offset, meta_size);

			input_ptr += meta_size;
			ctx_input.logical_offset += meta_size;
			circular_buf_notify_progress(&ctx_input);

			/* Reset for next SSTable */
			output_offset = 0;
			output_block_offset = 0;
			cumulative_output_block_size = 0;
			input_block_start = input_ptr;
			input_block_start_logical_offset = ctx_input.logical_offset;
			block_num = 0;  /* Reset block counter for new SSTable */

			CSD_DEBUG_MAGIC_CRC("Magic CRC[%d]: completed SSTable, total_output=%lu\n",
				   core_id, ctx_output.logical_write_offset);
		}
	}

	/* Copy remaining data page by page until compaction output is complete */
	while (!circular_buf_load_complete(&ctx_input)) {
		circular_buf_check_data(&ctx_input, SLM_PAGE_SIZE, pid, host_id);
		circular_buf_check_data_at(&ctx_input, ctx_input.logical_offset + SLM_PAGE_SIZE,
					   SLM_PAGE_SIZE, pid, host_id);
		if (circular_buf_load_complete(&ctx_input)) {
			break;
		}
		/* Copy this page */
		circular_output_set_data_from_circular(&ctx_output, &ctx_input,
				  ctx_input.logical_offset, SLM_PAGE_SIZE);
		ctx_input.logical_offset += SLM_PAGE_SIZE;
		circular_buf_notify_progress(&ctx_input);
	}

	/* Copy final leftover (partial page) */
	size_t final_input_size = ctx_input.lba_info->logical_total_size;
	if (ctx_input.logical_offset < final_input_size) {
		size_t remaining_size = final_input_size - ctx_input.logical_offset;
		CSD_DEBUG_MAGIC_CRC("Magic CRC[%d]: copying final leftovers, logical_offset=%lu, remaining=%lu\n",
				    core_id, ctx_input.logical_offset, remaining_size);
		circular_output_set_data_from_circular(&ctx_output, &ctx_input,
				  ctx_input.logical_offset, remaining_size);
		ctx_input.logical_offset += remaining_size;
	}

	/* Final cleanup */
	circular_buf_notify_progress(&ctx_input);
	circular_output_finalize(&ctx_output);

	NVMEV_CSD_PROFILE_REAL_END(pid, host_id);

#if (_MAGIC_TIME_BREAKDOWN_ == 1)
	{
		u64 crc_end_ns = ktime_get_ns();
		size_t output_bytes = ctx_output.logical_write_offset;
		u64 elapsed_us = (crc_end_ns - crc_start_ns) / 1000;
		printk(KERN_INFO "[CIRCULAR_CRC] core_id=%d time_us=%llu bytes=%lu us_per_MB=%llu\n",
		       core_id, elapsed_us, output_bytes,
		       elapsed_us * 1024 * 1024 / (output_bytes + 1));
	}
#endif

	CSD_DEBUG_MAGIC_CRC("Magic CRC: completed, total_output=%lu\n",
		   ctx_output.logical_write_offset);

	return ctx_output.logical_write_offset;
}

/**
 * search_data_block_binary - Binary search through a data block for target key
 * @data_ptr: Pointer to the start of the data block content
 * @data_size: Size of the data block including trailer
 * @target_key: The user key to search for (without internal bytes)
 * @target_key_len: Length of the target key
 * @info: Output parameter for decode_info of found entry
 * @found_key_ptr: Output parameter for pointer to found key data
 *
 * Returns: true if key found, false otherwise
 *
 * Assumes delta encoding is OFF (shared_len = 0), keys are sorted,
 * and all entries have identical size.
 */
static bool search_data_block_binary(char *data_ptr, uint64_t data_size,
				     char *target_key, int target_key_len,
				     struct decode_info *info, char **found_key_ptr)
{
	char *ptr;
	char *limit_ptr;
	uint32_t num_restarts;
	uint64_t restarts_offset;
	uint64_t entry_size;
	int64_t left, right, mid, num_entries;
	int key_len, cmp;

	num_restarts = 1;
	restarts_offset = data_size - (num_restarts + 1) * 4;
	limit_ptr = data_ptr + restarts_offset;

	/* Decode first entry to get entry size (all entries identical size) */
	ptr = data_ptr;
	ptr = DecodeEntry(ptr, limit_ptr, &info->shared, &info->non_shared,
			  &info->value_length);
	if (ptr == NULL)
		return false;

	/* entry_size = header_size + key_size + value_size */
	entry_size = (ptr - data_ptr) + info->non_shared + info->value_length;
	num_entries = restarts_offset / entry_size;

	if (num_entries == 1) {
		/* Only one entry, no need for binary search */
		key_len = info->non_shared - kNumInternalBytes;
		if (key_len == target_key_len) {
			cmp = memcmp(ptr, target_key, key_len);
			if (cmp == 0) {
				*found_key_ptr = ptr;
				return true;
			}
		}
		return false;
	}

	left = 0;
	right = num_entries - 1;

	while (left <= right) {
		mid = left + (right - left) / 2;

		/* Jump to entry at index mid */
		ptr = data_ptr + mid * entry_size;
		ptr = DecodeEntry(ptr, limit_ptr, &info->shared, &info->non_shared,
				  &info->value_length);
		if (ptr == NULL)
			return false;
		
		/* Compare user keys (excluding internal bytes) */
		key_len = info->non_shared - kNumInternalBytes;
		if (key_len == target_key_len) {
			cmp = memcmp(ptr, target_key, key_len);
		} else {
			cmp = key_len - target_key_len;
			NVMEV_ERROR("Key length mismatch during binary search: %d vs %d\n",
					  key_len, target_key_len);
		}

		if (cmp == 0) {
			*found_key_ptr = ptr;
			return true;
		} else if (cmp < 0) {
			left = mid + 1;
		} else {
			right = mid - 1;
		}
	}
	return false;
}

/**
 * search_data_block_linear - Linear search through a data block for target key
 * @data_ptr: Pointer to the start of the data block content
 * @data_size: Size of the data block including trailer
 * @target_key: The user key to search for (without internal bytes)
 * @target_key_len: Length of the target key
 * @info: Output parameter for decode_info of found entry
 * @found_key_ptr: Output parameter for pointer to found key data
 *
 * Returns: true if key found, false otherwise
 *
 * Assumes delta encoding is OFF (shared_len = 0) and keys are sorted.
 */
static bool __maybe_unused search_data_block_linear(char *data_ptr, uint64_t data_size,
				     char *target_key, int target_key_len,
				     struct decode_info *info, char **found_key_ptr)
{
	char *ptr = data_ptr;
	char *limit_ptr;
	uint32_t num_restarts;
	uint64_t restarts_offset;
	int key_len, cmp;

	/* Read num_restarts from end of block (before trailer) */
	//num_restarts = DecodeFixed32(data_ptr + data_size - 4);
	num_restarts = 1;

	/* Calculate where entry data ends (start of restart array) */
	restarts_offset = data_size - (num_restarts + 1) * 4;
	limit_ptr = data_ptr + restarts_offset;
	/* Linear scan through entries */
	while (ptr < limit_ptr) {
		ptr = DecodeEntry(ptr, limit_ptr, &info->shared, &info->non_shared,
				  &info->value_length);
		if (ptr == NULL)
			return false;

		/* Compare user keys (excluding internal bytes) */
		key_len = info->non_shared - kNumInternalBytes;
		if (key_len == target_key_len) {
			cmp = memcmp(ptr, target_key, key_len);
			if (cmp == 0) {
				*found_key_ptr = ptr;
				return true;
			} else if (cmp > 0) {
				return false; /* Keys are sorted, won't find it */
			}
		}
		/* Move to next entry */
		ptr += info->non_shared + info->value_length;
	}
	return false;
}

size_t __magic_rocksdb_read(void *param)
{
	struct magic_params *temp = (struct magic_params *)param;
	struct rocksdb_magic_read_params *read = &temp->read;
	struct rocksdb_read_params *rp = &read->read_params;
	size_t result = 0;
	int i;

	int search_level = rp->search_level;
	struct decode_info info;
	int idx;

	char *file_ptr[4];
	char *index_ptr[4];
	char *restart_ptr[4];
	char *ptr;
	size_t index_size;

	char *filter_ptr;
	size_t filter_size;

	char value[256];
	char target_key[32];
	int target_key_len;
	uint64_t data_offset, data_size;
	uint32_t index_offset, restart_offset;
	char *value_ptr, *value_limit;

	int64_t left, right, mid;
	int cmp;
	char *limit_ptr;
	int key_len, min_len;

	bool is_found = false;
	struct slm_lba_info *file_info;

	struct rocksdb_read_output *read_output =
		(struct rocksdb_read_output *)read->output_buf;
	char *output_ptr = (char *)read->output_buf + sizeof(struct rocksdb_read_output);

	CSD_DEBUG_MAGIC_READ("START: num_files=%d, input_buf=%lu, output_buf=%lu, output_buf_size=%lu\n",
		read->num_files,
		get_slm_offset(read->input_buf) >> SLM_PAGE_SHIFT,
		get_slm_offset(read->output_buf) >> SLM_PAGE_SHIFT,
		read->output_buf_size);

	CSD_DEBUG_MAGIC_READ("read_params: search_level=%d, key_length=%d\n",
		rp->search_level, rp->key_length);
	for (i = 0; i < search_level; i++) {
		CSD_DEBUG_MAGIC_READ("  level[%d]: start_offset=%llu, total_size=%llu, index_offset=%llu, search_type=%u\n",
			i, rp->start_offset[i], rp->total_size[i],
			rp->index_offset[i], rp->search_type[i]);
	}

	memcpy(target_key, rp->key, rp->key_length);
	target_key_len = rp->key_length;
	CSD_DEBUG_MAGIC_READ("target key length: %d\n", target_key_len);

	for (i = 0; i < search_level; i++) {
		file_ptr[i] = (char *)read->input_buf + rp->start_offset[i];
		index_ptr[i] = file_ptr[i] + rp->index_offset[i];
		restart_ptr[i] = file_ptr[i] + rp->restart_offset[i];
	}

	for (i = 0; i < search_level; i++) {
		if (rp->search_type[i] == 0) {
			/* Full file: extent-mapped, demand-loaded */
			size_t file_buf_addr = read->input_buf + read->file_start_offset[i];
			file_info = get_slm_lba_info_from_addr(file_buf_addr);

			CSD_DEBUG_MAGIC_READ("Level %d: extent_mapped=%d, slm_page=%lu\n",
				i, file_info ? file_info->extent_mapped : -1,
				get_slm_offset(file_buf_addr) >> SLM_PAGE_SHIFT);

			/* Bloom filter check */
			if (rp->filter_size[i] > 0) {
				filter_ptr = file_ptr[i] + rp->filter_offset[i];
				filter_size = rp->filter_size[i];
				CSD_DEBUG_MAGIC_READ("Level %d: filter demand read: logical=%lu, size=%lu\n",
					i, get_slm_offset((size_t)filter_ptr) >> SLM_PAGE_SHIFT, filter_size);
				check_data_using_ptr_info((size_t)filter_ptr, filter_size, 0, 0, file_info);
				/* Translate filter pointer after demand load */
				if (file_info && file_info->extent_mapped) {
					char *old_filter = filter_ptr;
					filter_ptr = (char *)extent_translate_addr(file_info, (size_t)filter_ptr);
					CSD_DEBUG_MAGIC_READ("Level %d: filter translated: page %lu -> %lu\n",
						i, get_slm_offset((size_t)old_filter) >> SLM_PAGE_SHIFT,
						get_slm_offset((size_t)filter_ptr) >> SLM_PAGE_SHIFT);
				}
				if (!BloomMayMatch(target_key, target_key_len, filter_ptr, filter_size)) {
					CSD_DEBUG_MAGIC_READ("Level %d: Bloom - Negative\n", i);
					continue;
				}
				CSD_DEBUG_MAGIC_READ("Level %d: Bloom - Positive\n", i);
			}

			/* Index block: demand load and translate */
			index_size = rp->total_size[i] - rp->index_offset[i];
			CSD_DEBUG_MAGIC_READ("Level %d: index demand read: page=%lu, file_off=%lu, size=%lu\n",
				i, get_slm_offset((size_t)index_ptr[i]) >> SLM_PAGE_SHIFT,
				rp->index_offset[i], index_size);
			check_data_using_ptr_info((size_t)index_ptr[i], index_size, 0, 0, file_info);

			/* Translate index and restart pointers */
			char *real_index_ptr = index_ptr[i];
			char *real_restart_ptr = restart_ptr[i];
			if (file_info && file_info->extent_mapped) {
				real_index_ptr = (char *)extent_translate_addr(file_info, (size_t)index_ptr[i]);
				real_restart_ptr = (char *)extent_translate_addr(file_info, (size_t)restart_ptr[i]);
				CSD_DEBUG_MAGIC_READ("Level %d: index translated: page %lu -> %lu, restart: page %lu -> %lu\n",
					i, get_slm_offset((size_t)index_ptr[i]) >> SLM_PAGE_SHIFT,
					get_slm_offset((size_t)real_index_ptr) >> SLM_PAGE_SHIFT,
					get_slm_offset((size_t)restart_ptr[i]) >> SLM_PAGE_SHIFT,
					get_slm_offset((size_t)real_restart_ptr) >> SLM_PAGE_SHIFT);
			}
			limit_ptr = real_index_ptr + index_size;

			left = 0;
			right = rp->num_data_blocks[i];

			while (left < right) {
				mid = left + (right - left) / 2;

				ptr = real_restart_ptr + 4 * mid;
				restart_offset = DecodeFixed32(ptr);

				ptr = real_index_ptr + restart_offset;
				ptr = DecodeEntry(ptr, limit_ptr, &info.shared, &info.non_shared,
								  &info.value_length);
				if (ptr == NULL || info.shared != 0) {
					printk("Error: Invalid restart point at mid=%lld\n", mid);
					goto out;
				}

				key_len = info.non_shared;
				min_len = (key_len < target_key_len) ? key_len : target_key_len;
				cmp = memcmp(ptr, target_key, min_len);
				if (cmp == 0) {
					cmp = (key_len - target_key_len);
				}

				if (cmp < 0) {
					left = mid + 1;
				} else if (cmp > 0) {
					right = mid;
				} else {
					left = right = mid;
				}
			}

			idx = (uint32_t)left;
			CSD_DEBUG_MAGIC_READ("Level %d: binary search idx=%d\n", i, idx);

			ptr = real_restart_ptr + 4 * idx;
			index_offset = DecodeFixed32(ptr);

			ptr = real_index_ptr + index_offset;
			ptr = DecodeEntry(ptr, limit_ptr, &info.shared, &info.non_shared,
							  &info.value_length);
			ptr += info.non_shared;
			memcpy(value, ptr, info.value_length);

			value_ptr = value;
			value_limit = value + info.value_length;
			value_ptr = GetVarint64Ptr(value_ptr, value_limit, &data_offset);
			value_ptr = GetVarint64Ptr(value_ptr, value_limit, &data_size);
			CSD_DEBUG_MAGIC_READ("Level %d: data block offset=%llu, size=%llu\n",
				i, data_offset, data_size);

			/* Data block: demand load and translate */
			CSD_DEBUG_MAGIC_READ("Level %d: data block demand read: logical=%lu, size=%llu\n",
				i, get_slm_offset((size_t)(file_ptr[i] + data_offset)) >> SLM_PAGE_SHIFT, data_size);
			check_data_using_ptr_info((size_t)(file_ptr[i] + data_offset), data_size, 0, 0, file_info);
			char *real_data_ptr = file_ptr[i] + data_offset;
			if (file_info && file_info->extent_mapped) {
				real_data_ptr = (char *)extent_translate_addr(file_info, (size_t)(file_ptr[i] + data_offset));
				CSD_DEBUG_MAGIC_READ("Level %d: data block translated: page %lu -> %lu\n",
					i, get_slm_offset((size_t)(file_ptr[i] + data_offset)) >> SLM_PAGE_SHIFT,
					get_slm_offset((size_t)real_data_ptr) >> SLM_PAGE_SHIFT);
			}

			char *found_key_ptr;
			if (search_data_block_binary(real_data_ptr, data_size,
						     target_key, target_key_len,
						     &info, &found_key_ptr)) {
				CSD_DEBUG_MAGIC_READ("Found key at level %d\n", i);
				ptr = found_key_ptr;
				key_len = info.non_shared;
				is_found = true;
				break;
			}
		} else {
			/* Partial load: search data block directly (not extent-mapped) */
			data_offset = rp->block_offset[i];
			data_size = rp->block_size[i];
			CSD_DEBUG_MAGIC_READ("Level %d: direct data block offset=%llu, size=%llu\n",
				i, data_offset, data_size);

			check_data_using_ptr((size_t)file_ptr[i], data_size, 0, 0);

			char *data_block_ptr = file_ptr[i] + (data_offset - (data_offset / 512 * 512));
			char *found_key_ptr;
			if (search_data_block_binary(data_block_ptr, data_size,
						     target_key, target_key_len,
						     &info, &found_key_ptr)) {
				CSD_DEBUG_MAGIC_READ("Found key at level %d\n", i);
				ptr = found_key_ptr;
				key_len = info.non_shared;
				is_found = true;
				break;
			} else {
				CSD_DEBUG_MAGIC_READ("Key not found in data block at level %d\n", i);
				continue;
			}
		}
	}

out:
	if (is_found) {
		read_output->key_length = key_len - kNumInternalBytes;
		read_output->value_length = info.value_length;
		read_output->sequence_number =
			DecodeFixed64(ptr + info.non_shared - kNumInternalBytes) >> 8;
		memcpy(output_ptr, ptr, read_output->key_length);
		ptr += info.non_shared;
		memcpy(output_ptr + read_output->key_length, ptr, read_output->value_length);
	} else {
		read_output->key_length = 0;
		read_output->value_length = 0;
		read_output->sequence_number = 0;
	}

	result = sizeof(struct rocksdb_read_output) + read_output->key_length +
		   read_output->value_length;

	/* Finalize output so host can read it */
	finalize_slm_data_ready(read->output_buf + result, result);
	notify_slm_data_ready(read->output_buf + result, 16384);

	CSD_DEBUG_MAGIC_READ("DONE: result=%lu, found=%d\n", result, is_found);
	return result;
}

/* ============================================================================
 * __rocksdb_mvcc_filter -- naive/sync baseline offload of RocksDB's
 * DBIter::FindNextUserEntry MVCC-version-skipping logic, ONE SST FILE AT A TIME.
 *
 * Unlike __rocksdb_compaction/__rocksdb_read above, this kernel does NOT assume
 * this module's own simplified SST layout (fixed-width keys, shared==0, inline
 * restart sentinels). buf_in is the WHOLE, ordinary RocksDB SST file exactly as
 * written by a real (format_version=6) BlockBasedTable writer, and this kernel
 * parses its footer, metaindex block, and index block itself to find data
 * blocks. See rocksdb_mvcc_filter_params / rocksdb_mvcc_filter_output in
 * csd_user_func.h for the calling convention.
 *
 * Scope (deliberately naive):
 *   - one SST file per call, no cross-file/cross-level merge (that stays the job
 *     of the host's unmodified MergingIterator + DBIter::FindNextUserEntry);
 *   - synchronous, whole-file-already-loaded (no async/dependency-table use).
 *
 * Filtering rules per internal key-value entry:
 *   1. Visibility: drop entries whose sequence number is above snapshot_seq.
 *   2. Same-user-key dedup: within one file, keys are sorted with descending
 *      sequence number per user key, so the first visible entry for a user key
 *      is the newest one; drop every subsequent entry for that same user key
 *      (this correctly leaves a visible tombstone in the output -- it must NOT
 *      be dropped, since a merged multi-file scan needs to see it to shadow an
 *      older visible version of the same key living in a different SST).
 *   3. kTypeMerge entries always pass through and never participate in dedup
 *      bookkeeping (every merge operand must reach the host).
 * ============================================================================
 */

#define MVCC_FOOTER_SIZE 53
#define MVCC_FORMAT_VERSION 6 /* BlockBasedTable writer, MySQL 8.4-era RocksDB */
#define MVCC_TYPE_MERGE 0x2 /* rocksdb::ValueType::kTypeMerge */

/* Standard RocksDB block trailer: [entries...][restart_pt_0]...[restart_pt_{n-1}]
 * [num_restarts], each restart_pt/num_restarts a fixed32. Returns a pointer to
 * where the restart-point array begins (i.e. one past the last real entry), or
 * NULL if the block is too small to even hold the num_restarts field. */
static char *mvcc_block_restart_start(char *block_start, size_t block_size)
{
	char *block_end;
	uint32_t num_restarts;

	if (block_size < 4)
		return NULL;
	block_end = block_start + block_size;
	num_restarts = DecodeFixed32(block_end - 4);
	if ((size_t)num_restarts * 4 + 4 > block_size)
		return NULL; /* corrupt: restart array would extend before block_start */
	return block_end - 4 - ((size_t)num_restarts * 4);
}

/* Decode one standard block entry ([shared][non_shared][value_length][key_delta]
 * [value]) -- used for metaindex-block and data-block entries -- and reconstruct
 * the full key into key_buf, carrying the shared prefix forward from whatever was
 * already in key_buf/*key_len (caller resets *key_len=0 at the start of each new
 * block, since delta-encoding never crosses a block boundary). */
static char *mvcc_decode_block_entry(char *p, char *limit, char *key_buf, size_t key_buf_cap,
									 uint32_t *key_len, char **value_ptr, uint32_t *value_len)
{
	uint32_t shared, non_shared, vlen;
	char *key_delta = DecodeEntry(p, limit, &shared, &non_shared, &vlen);

	if (key_delta == NULL)
		return NULL;
	if (shared > *key_len)
		return NULL; /* corrupt: shared prefix longer than the previous key */
	if ((size_t)shared + non_shared > key_buf_cap)
		return NULL; /* key too long for our scratch buffer -- bail out safely */
	memcpy(key_buf + shared, key_delta, non_shared);
	*key_len = shared + non_shared;
	*value_ptr = key_delta + non_shared;
	*value_len = vlen;
	return *value_ptr + vlen;
}

/* Decode one INDEX-block entry. format_version >= 6 index entries OMIT the
 * value_length field that ordinary block entries have -- the value is a
 * BlockHandle {offset, size}, self-delimiting via its own two varints, so no
 * declared length is needed. Do NOT reuse mvcc_decode_block_entry()/DecodeEntry()
 * here, it assumes a value_length field that isn't present in this format. */
static char *mvcc_decode_index_entry(char *p, char *limit, char *key_buf, size_t key_buf_cap,
									 uint32_t *key_len, uint64_t *handle_offset,
									 uint64_t *handle_size)
{
	uint32_t shared, non_shared;
	char *key_delta, *vp;

	key_delta = GetVarint32Ptr(p, limit, &shared);
	if (key_delta == NULL)
		return NULL;
	key_delta = GetVarint32Ptr(key_delta, limit, &non_shared);
	if (key_delta == NULL)
		return NULL;
	if (shared > *key_len)
		return NULL;
	if ((size_t)shared + non_shared > key_buf_cap)
		return NULL;
	if ((limit - key_delta) < (int64_t)non_shared)
		return NULL;
	memcpy(key_buf + shared, key_delta, non_shared);
	*key_len = shared + non_shared;

	vp = key_delta + non_shared;
	vp = GetVarint64Ptr(vp, limit, handle_offset);
	if (vp == NULL)
		return NULL;
	vp = GetVarint64Ptr(vp, limit, handle_size);
	if (vp == NULL)
		return NULL;
	return vp;
}

/* Emit one already-filtered internal-key/value pair into the flat
 * [u32 key_len][key][u32 value_len][value] output stream via set_data_from_ptr()
 * (the SLM-aware write helper every other sync kernel in this file uses).
 * Returns false (and writes nothing) if it would overflow out_limit. */
static bool mvcc_emit_entry(char **out_ptr, char *out_limit, char *key, uint32_t key_len,
							char *value, uint32_t value_len)
{
	if ((size_t)(out_limit - *out_ptr) < (size_t)4 + key_len + 4 + value_len)
		return false;

	set_data_from_ptr((size_t)*out_ptr, (size_t)&key_len, sizeof(key_len));
	*out_ptr += sizeof(key_len);
	set_data_from_ptr((size_t)*out_ptr, (size_t)key, key_len);
	*out_ptr += key_len;
	set_data_from_ptr((size_t)*out_ptr, (size_t)&value_len, sizeof(value_len));
	*out_ptr += sizeof(value_len);
	set_data_from_ptr((size_t)*out_ptr, (size_t)value, value_len);
	*out_ptr += value_len;
	return true;
}

size_t __rocksdb_mvcc_filter(void *buf_in, void *buf_out, size_t size, void *param)
{
	struct CSD_PARAMS *temp = (struct CSD_PARAMS *)param;
	int pid = temp->profile_info.pid;
	int host_id = temp->profile_info.host_id;
	size_t file_len = temp->rocksdb_mvcc_filter_params.sstable_size;
	uint64_t snapshot_seq = temp->rocksdb_mvcc_filter_params.snapshot_seq;
	size_t output_capacity = temp->rocksdb_mvcc_filter_params.output_capacity;

	char *file_data = (char *)buf_in;
	struct rocksdb_mvcc_filter_output *out_header =
		(struct rocksdb_mvcc_filter_output *)buf_out;
	char *out_ptr = (char *)buf_out + sizeof(struct rocksdb_mvcc_filter_output);
	char *out_limit = (char *)buf_out + output_capacity;

	uint64_t keys_seen = 0, keys_filtered = 0;
	char *footer, *metaindex_start, *metaindex_restart_limit;
	uint64_t file_magic;
	uint32_t format_version, metaindex_size;
	uint64_t index_handle_offset = 0, index_handle_size = 0;
	bool found_index = false;

	char meta_key_buf[40];
	uint32_t meta_key_len;
	char index_key_buf[40];
	uint32_t index_key_len;
	char data_key_buf[48];
	uint32_t data_key_len;
	char prev_user_key[40];
	uint32_t prev_user_key_len = 0;
	bool have_prev_user_key = false;

	static const char kIndexMetaKey[] = "rocksdb.index";
	const uint32_t kIndexMetaKeyLen = sizeof(kIndexMetaKey) - 1;

	char *index_start, *index_restart_limit, *ip;

	NVMEV_CSD_PROFILE_REAL_START(pid, host_id);
	CSD_DEBUG("rocksdb_mvcc_filter: file_len=%zu snapshot_seq=%llu output_capacity=%zu\n",
			  file_len, (unsigned long long)snapshot_seq, output_capacity);

	if (output_capacity < sizeof(struct rocksdb_mvcc_filter_output)) {
		printk("nvmevirt mvcc_filter: output_capacity too small\n");
		goto done;
	}
	if (file_len < MVCC_FOOTER_SIZE || file_len > size) {
		printk("nvmevirt mvcc_filter: bad file_len %zu (buf size %zu)\n", file_len, size);
		goto done;
	}

	/* Wait for the whole file: the footer is at the end and is needed first.
	 * Probe one page at a time, not the whole range at once. The loader only
	 * refills its quota while stream_access is set, and that is set only by a
	 * probe at a non-zero offset, so a single whole-range probe stalls the
	 * load after the first page.
	 */
	{
		size_t off;

		for (off = 0; off < file_len; off += SLM_PAGE_SIZE)
			check_data_using_ptr((size_t)file_data + off, SLM_PAGE_SIZE, pid,
					     host_id);
	}

	footer = file_data + file_len - MVCC_FOOTER_SIZE;
	file_magic = DecodeFixed64(footer + 45);
	if (file_magic != kBlockBasedTableMagicNumber) {
		printk("nvmevirt mvcc_filter: bad magic 0x%llx\n", (unsigned long long)file_magic);
		goto done;
	}
	format_version = DecodeFixed32(footer + 41);
	if (format_version != MVCC_FORMAT_VERSION) {
		printk("nvmevirt mvcc_filter: unsupported format_version %u (expected %d)\n",
			   format_version, MVCC_FORMAT_VERSION);
		goto done;
	}
	metaindex_size = DecodeFixed32(footer + 13);
	if (metaindex_size == 0 || (size_t)metaindex_size + kBlockTrailerSize + MVCC_FOOTER_SIZE > file_len) {
		printk("nvmevirt mvcc_filter: bad metaindex_size %u\n", metaindex_size);
		goto done;
	}
	metaindex_start = footer - kBlockTrailerSize - metaindex_size;

	/* --- scan the metaindex block for the "rocksdb.index" handle --- */
	metaindex_restart_limit = mvcc_block_restart_start(metaindex_start, metaindex_size);
	if (metaindex_restart_limit == NULL || metaindex_restart_limit < metaindex_start) {
		printk("nvmevirt mvcc_filter: bad metaindex restart array\n");
		goto done;
	}
	{
		char *p = metaindex_start;

		meta_key_len = 0;
		while (p < metaindex_restart_limit) {
			char *value_ptr;
			uint32_t value_len;
			char *next = mvcc_decode_block_entry(p, metaindex_restart_limit, meta_key_buf,
												 sizeof(meta_key_buf), &meta_key_len,
												 &value_ptr, &value_len);
			if (next == NULL) {
				printk("nvmevirt mvcc_filter: metaindex parse error\n");
				break;
			}
			if (meta_key_len == kIndexMetaKeyLen &&
				memcmp(meta_key_buf, kIndexMetaKey, kIndexMetaKeyLen) == 0) {
				char *vp = GetVarint64Ptr(value_ptr, value_ptr + value_len, &index_handle_offset);
				if (vp != NULL)
					vp = GetVarint64Ptr(vp, value_ptr + value_len, &index_handle_size);
				found_index = (vp != NULL);
				break;
			}
			p = next;
		}
	}
	if (!found_index) {
		printk("nvmevirt mvcc_filter: rocksdb.index handle not found in metaindex\n");
		goto done;
	}
	if (index_handle_offset + index_handle_size > file_len) {
		printk("nvmevirt mvcc_filter: index handle out of bounds\n");
		goto done;
	}

	/* --- walk the index block; filter each referenced data block as we go --- */
	index_start = file_data + index_handle_offset;
	index_restart_limit = mvcc_block_restart_start(index_start, index_handle_size);
	if (index_restart_limit == NULL || index_restart_limit < index_start) {
		printk("nvmevirt mvcc_filter: bad index restart array\n");
		goto done;
	}

	index_key_len = 0;
	ip = index_start;
	while (ip < index_restart_limit) {
		uint64_t data_offset, data_size;
		char *block_start, *block_restart_limit, *bp;
		char *next = mvcc_decode_index_entry(ip, index_restart_limit, index_key_buf,
											 sizeof(index_key_buf), &index_key_len,
											 &data_offset, &data_size);
		if (next == NULL) {
			printk("nvmevirt mvcc_filter: index block parse error\n");
			break;
		}
		ip = next;

		if (data_offset + data_size > file_len) {
			printk("nvmevirt mvcc_filter: data block out of bounds\n");
			break;
		}

		/* --- filter one data block --- */
		block_start = file_data + data_offset;
		block_restart_limit = mvcc_block_restart_start(block_start, data_size);
		if (block_restart_limit == NULL || block_restart_limit < block_start) {
			printk("nvmevirt mvcc_filter: bad data-block restart array, skipping block\n");
			continue;
		}

		data_key_len = 0; /* delta-decode state resets at the start of every block */
		bp = block_start;
		while (bp < block_restart_limit) {
			char *value_ptr;
			uint32_t value_len;
			uint64_t suffix, seq;
			uint8_t vtype;
			uint32_t user_key_len;
			bool emit = false;
			char *bnext = mvcc_decode_block_entry(bp, block_restart_limit, data_key_buf,
												  sizeof(data_key_buf), &data_key_len,
												  &value_ptr, &value_len);
			if (bnext == NULL) {
				printk("nvmevirt mvcc_filter: data block parse error\n");
				break;
			}
			bp = bnext;

			if (data_key_len < kNumInternalBytes) {
				printk("nvmevirt mvcc_filter: internal key too short (%u)\n", data_key_len);
				continue;
			}

			keys_seen++;
			suffix = DecodeFixed64(data_key_buf + data_key_len - kNumInternalBytes);
			seq = suffix >> 8;
			vtype = (uint8_t)(suffix & 0xff);
			user_key_len = data_key_len - kNumInternalBytes;

			if (seq > snapshot_seq) {
				/* Rule 1: invisible to this snapshot */
				keys_filtered++;
			} else if (vtype == MVCC_TYPE_MERGE) {
				/* Rule 3: merge operands always pass through untouched, and never
				 * participate in same-key dedup (every operand must reach the host) */
				emit = true;
			} else if (have_prev_user_key && prev_user_key_len == user_key_len &&
					   memcmp(prev_user_key, data_key_buf, user_key_len) == 0) {
				/* Rule 2: older version of a user key we already kept the newest
				 * visible entry for (including a tombstone -- which we DO emit
				 * below, since the host's cross-file merge needs to see it) */
				keys_filtered++;
			} else {
				emit = true;
				memcpy(prev_user_key, data_key_buf, user_key_len);
				prev_user_key_len = user_key_len;
				have_prev_user_key = true;
			}

			if (emit) {
				if (!mvcc_emit_entry(&out_ptr, out_limit, data_key_buf, data_key_len,
									 value_ptr, value_len)) {
					printk("nvmevirt mvcc_filter: output buffer full, truncating\n");
					goto scan_done;
				}
			}
		}
	}

scan_done:
done:
	if ((char *)buf_out + sizeof(struct rocksdb_mvcc_filter_output) <= out_limit) {
		out_header->keys_seen = keys_seen;
		out_header->keys_filtered = keys_filtered;
	}
	NVMEV_CSD_PROFILE_REAL_END(pid, host_id);
	return (size_t)(out_ptr - (char *)buf_out);
}

size_t __rocksdb_read(void *buf_in, void *buf_out, size_t size, void *param)
{
	size_t result = 0;
	struct CSD_PARAMS *temp = (struct CSD_PARAMS *)param;
	int pid = temp->profile_info.pid;
	int host_id = temp->profile_info.host_id;
	int search_level = temp->rocksdb_read_params.search_level;
	int i, j;
	struct decode_info info;
	int idx;

	char *file_ptr[4];
	char *index_ptr[4];
	char *restart_ptr[4];
	char *ptr;
	size_t index_size;

	char *filter_ptr;
	size_t filter_size;

	char value[256];
	char target_key[32];
	int target_key_len;
	uint64_t data_offset, data_size;
	uint32_t index_offset, restart_offset;
	char *value_ptr, *value_limit;
	uint64_t sequence_number;

	int64_t left, right, mid;
	int cmp;
	char *limit_ptr;
	int key_len, min_len;

	bool is_found = false;

	struct rocksdb_read_output *read_output = (struct rocksdb_read_output *)buf_out;
	char *output_ptr = (char *)buf_out + sizeof(struct rocksdb_read_output);

	memcpy(target_key, temp->rocksdb_read_params.key,
		   temp->rocksdb_read_params.key_length);
	target_key_len = temp->rocksdb_read_params.key_length;
	READ_DEBUG("Target key to search: %s (len %d)\n", target_key, target_key_len);

	// for (i = 0; i < search_level; i++) {
	// 	READ_DEBUG(
	// 		"level %d: index_offset %lu, restart_offset %lu, size %lu, num_data_blocks %d, start_offset %lu, block_offset %lu, block_size %lu, search_type %d\n",
	// 		i, temp->rocksdb_read_params.index_offset[i],
	// 		temp->rocksdb_read_params.restart_offset[i],
	// 		temp->rocksdb_read_params.total_size[i],
	// 		temp->rocksdb_read_params.num_data_blocks[i],
	// 		temp->rocksdb_read_params.start_offset[i],
	// 		temp->rocksdb_read_params.block_offset[i],
	// 		temp->rocksdb_read_params.block_size[i],
	// 		temp->rocksdb_read_params.search_type[i]);
	// }

	for (i = 0; i < search_level; i++) {
		file_ptr[i] = (char *)buf_in + temp->rocksdb_read_params.start_offset[i];
		index_ptr[i] = file_ptr[i] + temp->rocksdb_read_params.index_offset[i];
		restart_ptr[i] = file_ptr[i] + temp->rocksdb_read_params.restart_offset[i];
	}

	for (i = 0; i < search_level; i++) {
		if (temp->rocksdb_read_params.search_type[i] == 0) {
			// check if filter block exists
			if (temp->rocksdb_read_params.filter_size[i] > 0) {
				// check bloom filter
				filter_ptr = file_ptr[i] + temp->rocksdb_read_params.filter_offset[i];
				filter_size = temp->rocksdb_read_params.filter_size[i];
				check_data_using_ptr((size_t)filter_ptr, filter_size, pid, host_id);
				if (!BloomMayMatch(target_key, target_key_len, filter_ptr, filter_size)) {
					READ_DEBUG("Level %d: Bloom - Negative\n", i);
					continue;  // Skip to next level, key definitely not in this SSTable
				}
				READ_DEBUG("Level %d: Bloom - Positive\n", i);
			}

			// start from index block to find data block
			index_size = temp->rocksdb_read_params.total_size[i] -
						 temp->rocksdb_read_params.index_offset[i];
			READ_DEBUG("Level %d, key: %s, index: %lu\n", i, target_key, index_size);
			check_data_using_ptr((size_t)index_ptr[i], index_size, pid, host_id);
			READ_DEBUG("## Got Entire Index Block\n");
			ptr = restart_ptr[i] + 4 * idx;
			limit_ptr = file_ptr[i] + temp->rocksdb_read_params.total_size[i];

			left = 0;
			right = temp->rocksdb_read_params.num_data_blocks[i];

			while (left < right) {
				mid = left + (right - left) / 2;

				// Get restart point offset
				ptr = restart_ptr[i] + 4 * mid;
				restart_offset = DecodeFixed32(ptr);

				// Decode key at restart point
				ptr = index_ptr[i] + restart_offset;
				ptr = DecodeEntry(ptr, limit_ptr, &info.shared, &info.non_shared,
								  &info.value_length);
				if (ptr == NULL || info.shared != 0) {
					printk("Error: Invalid restart point at mid=%lld\n", mid);
					return 0;
				}

				// Compare with target key (excluding internal bytes)
				key_len = info.non_shared; // index block keeps user key only
				min_len = (key_len < target_key_len) ? key_len : target_key_len;
				cmp = memcmp(ptr, target_key, min_len);
				if (cmp == 0) {
					cmp = (key_len - target_key_len);
				}

				// printk("  mid=%lld, restart_offset=%u, key=%.*s(%d), cmp=%d\n",
				//     mid, restart_offset, key_len, ptr, key_len,cmp);

				if (cmp < 0) {
					left = mid + 1;  // key[mid] < target, search right
				} else if (cmp > 0) {
					right = mid;     // key[mid] > target, search left (inclusive)
				} else {
					left = right = mid;  // exact match
				}
			}

			idx = (uint32_t)left;
			READ_DEBUG("Binary search result: idx=%d\n", idx);

			/* Get corresponding index entry */
			ptr = restart_ptr[i] + 4 * idx;
			index_offset = DecodeFixed32(ptr);

			/* Decode index entry and get data block offset */
			ptr = index_ptr[i] + index_offset;
			ptr = DecodeEntry(ptr, limit_ptr, &info.shared, &info.non_shared,
							  &info.value_length);
			ptr += info.non_shared;
			memcpy(value, ptr, info.value_length);

			value_ptr = value;
			value_limit = value + info.value_length;
			value_ptr = GetVarint64Ptr(value_ptr, value_limit, &data_offset);
			value_ptr = GetVarint64Ptr(value_ptr, value_limit, &data_size);
			READ_DEBUG("Data block: offset=%llu, size=%llu\n", data_offset, data_size);

			/* Linear search through data block entries */
			check_data_using_ptr((size_t)file_ptr[i] + data_offset, data_size, pid, host_id);
			READ_DEBUG("## Got Data Block\n");

			char *found_key_ptr;
			if (search_data_block_binary(file_ptr[i] + data_offset, data_size,
						     target_key, target_key_len,
						     &info, &found_key_ptr)) {
				READ_DEBUG("Found key at level %d: %s\n", i, target_key);
				ptr = found_key_ptr;
				key_len = info.non_shared;
				is_found = true;
				break;
			}
		} else {
			/* Start from data block directly
			 * Logic is different from above because we only load the
			 * data block at host side */
			data_offset = temp->rocksdb_read_params.block_offset[i];
			data_size = temp->rocksdb_read_params.block_size[i];
			READ_DEBUG("Direct search - Data block: offset=%llu, size=%llu\n",
				   data_offset, data_size);

			check_data_using_ptr((size_t)file_ptr[i], data_size, pid, host_id);
			READ_DEBUG("## Got Data Block\n");

			/* Calculate actual data block start within the loaded buffer */
			char *data_block_ptr = file_ptr[i] + (data_offset - (data_offset / 512 * 512));
			char *found_key_ptr;
			if (search_data_block_binary(data_block_ptr, data_size,
						     target_key, target_key_len,
						     &info, &found_key_ptr)) {
				READ_DEBUG("Found key at level %d: %s\n", i, target_key);
				ptr = found_key_ptr;
				key_len = info.non_shared;
				is_found = true;
				break;
			}
		}
	}

	if (is_found) {
		read_output->key_length = key_len - kNumInternalBytes;
		read_output->value_length = info.value_length;
		read_output->sequence_number =
			DecodeFixed64(ptr + info.non_shared - kNumInternalBytes) >> 8;
		memcpy(output_ptr, ptr, read_output->key_length);
		ptr += info.non_shared;
		memcpy(output_ptr + read_output->key_length, ptr, read_output->value_length);
	} else {
		read_output->key_length = 0;
		read_output->value_length = 0;
		read_output->sequence_number = 0;
		READ_DEBUG("Key not found: %s\n", target_key);
	}

	return sizeof(struct rocksdb_read_output) + read_output->key_length +
		   read_output->value_length;
}