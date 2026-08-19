#ifndef _CSD_USER_FUNC_H
#define _CSD_USER_FUNC_H

#ifndef __KERNEL__
#include <stdint.h>
#endif

#undef CONFIG_CSD_FUNC_VERBOSE

#ifdef CONFIG_CSD_FUNC_VERBOSE
#define CSD_DEBUG(string, args...) printk(KERN_INFO string, ##args)
#else
#define CSD_DEBUG(string, args...)
#endif

#undef CONFIG_READ_FUNC_VERBOSE
// #define CONFIG_READ_FUNC_VERBOSE

#ifdef CONFIG_READ_FUNC_VERBOSE
#define READ_DEBUG(string, args...) printk(KERN_INFO string, ##args)
#else
#define READ_DEBUG(string, args...)
#endif

#define _MAGIC_DEBUG_COMPACTION_ (0)
#define _MAGIC_DEBUG_READ_ (0)
#define _MAGIC_DEBUG_INPUT_ (0)
#define _MAGIC_DEBUG_OUTPUT_ (0)
#define _MAGIC_DEBUG_CRC_ (0)
#define _MAGIC_TIME_BREAKDOWN_ (0)

#if (_MAGIC_DEBUG_COMPACTION_ == 1)
#define CSD_DEBUG_MAGIC_COMPACTION(string, args...) printk("[MAGIC_COMPACTION] - %s : " string, __func__, ##args)
#else
#define CSD_DEBUG_MAGIC_COMPACTION(string, args...)
#endif

#if (_MAGIC_DEBUG_READ_ == 1)
#define CSD_DEBUG_MAGIC_READ(string, args...) printk("[MAGIC_READ][CPU%d] - %s : " string, smp_processor_id(), __func__, ##args)
#else
#define CSD_DEBUG_MAGIC_READ(string, args...)
#endif

#if (_MAGIC_DEBUG_INPUT_ == 1)
#define CSD_DEBUG_MAGIC_INPUT(string, args...) printk("[MAGIC_INPUT] - %s : " string, __func__, ##args)
#define CSD_DEBUG_MAGIC_INPUT_CTX(ctx, string, args...) printk("[MAGIC_INPUT:%s] - %s : " string, ctx, __func__, ##args)
#else
#define CSD_DEBUG_MAGIC_INPUT(string, args...)
#define CSD_DEBUG_MAGIC_INPUT_CTX(ctx, string, args...)
#endif

#if (_MAGIC_DEBUG_OUTPUT_ == 1)
#define CSD_DEBUG_MAGIC_OUTPUT(string, args...) printk("[MAGIC_OUTPUT] - %s : " string, __func__, ##args)
#define CSD_DEBUG_MAGIC_OUTPUT_CTX(ctx, string, args...) printk("[MAGIC_OUTPUT:%s] - %s : " string, ctx, __func__, ##args)
#else
#define CSD_DEBUG_MAGIC_OUTPUT(string, args...)
#define CSD_DEBUG_MAGIC_OUTPUT_CTX(ctx, string, args...)
#endif

#if (_MAGIC_DEBUG_CRC_ == 1)
#define CSD_DEBUG_MAGIC_CRC(string, args...) printk("[MAGIC_CRC] - %s : " string, __func__, ##args)
#else
#define CSD_DEBUG_MAGIC_CRC(string, args...)
#endif

#define ALIGNED_UP(value, align) (((value) + (align)-1) & ~((align)-1))

enum CSD_PROGRAM_INDEX {
	COPY_TO_SLM_PROGRAM_INDEX = 0,

	STREAM_TYPE_PROGRAM_INDEX_START = 1,
	KEY_FINDER_PROGRAM_INDEX, // 2
	KNN_PROGRAM_INDEX, // 3
	GREP_PROGRAM_INDEX, // 4
	TPCH_FILTER_INDEX, // 5
	DECOMPRESSION_INDEX, // 6
	TPCH_FILTER2_INDEX,
	BITMAP_PROGRAM_INDEX,
	STATIC_OUTPUT_TYPE_PROGRAM_INDEX_START,
	STAT32_PROGRAM_INDEX, // 7
	TPCH_STAT_PROGRAM_INDEX, // 8
	FILTERED_STAT32_PROGRAM_INDEX, // 9
	STATIC_OUTPUT_TYPE_PROGRAM_INDEX_END,
	STREAM_TYPE_PROGRAM_INDEX_END,

	RANDOM_ACCESS_TYPE_PROGRAM_INDEX_START = 0x100,
	BTREE_PROGRAM_INDEX, // 257
	BTREE_PROGRAM_INDEX_TEMP, // 258
	QUICK_SORT_INDEX, // 6
	ROCKSDB_READ_PROGRAM_INDEX,
	RANDOM_ACCESS_TYPE_PROGRAM_INDEX_END,

	STORE_TYPE_PROGRAM_INDEX_START = 0x200,
	COMPRESSION_INDEX, // 513
	STORE_TYPE_PROGRAM_INDEX_END,

	MULTI_STREAM_TYPE_PROGRAM_INDEX_START = 0x300,
	COMPACTION_PROGRAM_INDEX,
	LEVELDB_COMPACTION_PROGRAM_INDEX,
	LEVELDB_CRC_PROGRAM_INDEX,
	ROCKSDB_COMPACTION_PROGRAM_INDEX,
	ROCKSDB_CRC_PROGRAM_INDEX,
	ROCKSDB_MVCC_FILTER_PROGRAM_INDEX, // naive/sync FindNextUserEntry-offload baseline
	HYBRID_ENCODING_PROGRAM_INDEX,
	HYBRID_DECODING_PROGRAM_INDEX,
	ROCKSDB_MAGIC_COMPACTION_PROGRAM_INDEX,
	ROCKSDB_MAGIC_CRC_PROGRAM_INDEX,
	ROCKSDB_MAGIC_READ_PROGRAM_INDEX,
	MULTI_STREAM_TYPE_PROGRAM_INDEX_END,

	eBPF_PROGRAM_INDEX = 0x10000,
};

#define IS_STREAM_TYPE_PROGRAM_INDEX(idx) \
	(((idx) > STREAM_TYPE_PROGRAM_INDEX_START) && ((idx) < STREAM_TYPE_PROGRAM_INDEX_END))
#define IS_STATIC_OUTPUT_TYPE_PROGRAM_INDEX(idx) \
	(((idx) > STATIC_OUTPUT_TYPE_PROGRAM_INDEX_START) && ((idx) < STATIC_OUTPUT_TYPE_PROGRAM_INDEX_END))
#define IS_RANDOM_ACCESS_PROGRAM_INDEX(idx) \
	(((idx) > RANDOM_ACCESS_TYPE_PROGRAM_INDEX_START) && ((idx) < RANDOM_ACCESS_TYPE_PROGRAM_INDEX_END))
#define IS_STORE_PROGRAM_INDEX(idx) (((idx) > STORE_TYPE_PROGRAM_INDEX_START) && ((idx) < STORE_TYPE_PROGRAM_INDEX_END))
#define IS_MULTI_STREAM_PROGRAM_INDEX(idx) \
	(((idx) > MULTI_STREAM_TYPE_PROGRAM_INDEX_START) && ((idx) < MULTI_STREAM_TYPE_PROGRAM_INDEX_END))
#define IS_AUTO_OUTPUT_RECORD_PROGRAM_INDEX(idx) \
	((idx) == KEY_FINDER_PROGRAM_INDEX || (idx) == STAT32_PROGRAM_INDEX || (idx) == FILTERED_STAT32_PROGRAM_INDEX || (idx) == KNN_PROGRAM_INDEX || (idx) == GREP_PROGRAM_INDEX || (idx) == BITMAP_PROGRAM_INDEX || (idx) == ROCKSDB_READ_PROGRAM_INDEX)

// Parameter
struct ebpf_context {
	void *buf_o;
	void *buf_i;
	size_t size_i;
};

struct key_finder_params {
	unsigned int field;
	unsigned int value;
};
struct key_finder2_params {
	unsigned int field1;
	unsigned int value1;
	unsigned int field2;
	unsigned int value2;
};

struct tpch_filter_params {
	unsigned int value1;
	unsigned int value2;
};

struct knn_params {
	unsigned int row_length;
	unsigned int item_size;
};

struct grep_params {
	unsigned int column_size;
	unsigned int str_len;
	unsigned char str[16];
};

struct bitmap_params {
	unsigned int input_size;
};


struct btree_params {
	size_t key;
};

struct compression_params {
	int compress_type;
};

struct decompression_params {
	int decompress_type;
};

struct tpch_params {
	int tpch_num;
};

struct compaction_params {
	unsigned int nInput;
	unsigned int key_size;
	unsigned int value_size;
};

struct leveldb_compaction_params {
	size_t first_level_size;
	size_t second_level_start;
	size_t second_level_size;
	size_t sstable_size;
	size_t datablock_threshold;
	bool crc_flag; // crc flag for datablock crc calculation
};

struct leveldb_crc_params {
	size_t sstable_size;
	size_t datablock_threshold;
	int num_cores;
	int core_id;
};

struct rocksdb_host_properties_param {
	uint64_t file_number;
	uint64_t creation_time;
	uint64_t file_creation_time;
	uint64_t oldest_key_time;
	char db_id[37];
	char db_session_id[21];
};
struct rocksdb_compaction_params {
	size_t first_level_size;
	size_t second_level_start;
	size_t second_level_size;
	size_t sstable_size;
	size_t datablock_threshold;
	int block_size;
	bool crc_flag; // crc flag for datablock crc calculation
	int bits_per_key;
	bool bloom_flag; // bloom flag for bloom filter creation
	struct rocksdb_host_properties_param host_properties;
};

struct rocksdb_crc_params {
	size_t sstable_size;
	size_t datablock_threshold;
	int num_cores;
	int core_id;
};

struct magic_info {
	size_t output_buf;
	size_t output_second_buf;
	size_t output_buf_size;
	size_t output_second_buf_size;
};

struct rocksdb_magic_compaction_params {
	size_t first_level_start;
	size_t first_level_size;
	size_t second_level_start;
	size_t second_level_size;
	size_t sstable_size;
	size_t datablock_threshold;
	int block_size;
	bool crc_flag;
	int crc_cores;  // Number of CRC cores: 1 = single CRC task, 2 = chained CRC tasks
	int bits_per_key;
	bool bloom_flag;
	struct rocksdb_host_properties_param host_properties;
};

struct rocksdb_magic_crc_params {
	size_t sstable_size;
	size_t datablock_threshold;
	size_t input_buf;
	size_t output_buf;
	size_t output_buf_size;
	size_t input_logical_size;
	int num_cores;
	int core_id;
};

struct decoding_params {
	size_t original_size;
	int max_value;
};

struct rocksdb_read_params {
	uint64_t index_offset[4];
	uint64_t restart_offset[4];
	uint64_t total_size[4];
	uint64_t start_offset[4];
	uint64_t block_offset[4];
	uint64_t filter_offset[4];
	int block_size[4];
	int filter_size[4];
	int num_data_blocks[4];
	int search_level;
	char key[24];
	int key_length;
	uint16_t search_type[4]; // 0: index->data, 1: data
};

// Naive/sync FindNextUserEntry-offload baseline (ROCKSDB_MVCC_FILTER_PROGRAM_INDEX).
// buf_in is the WHOLE raw SST file (host loads it verbatim -- no host-side offset
// pre-computation, unlike rocksdb_read_params); the kernel parses the footer,
// metaindex block, and index block itself to find data blocks. See
// __rocksdb_mvcc_filter() in csd_user_func.c.
struct rocksdb_mvcc_filter_params {
	size_t sstable_size; // actual (unaligned) SST file size -- do not use the
			      // SLM/DMA-aligned buffer size for this
	uint64_t snapshot_seq; // read snapshot sequence number; entries with a higher
				// internal sequence number are filtered out
	size_t output_capacity; // total capacity of buf_out, including the
				 // rocksdb_mvcc_filter_output header
};

// Header written at the start of buf_out by __rocksdb_mvcc_filter(), followed by
// a flat stream of [uint32 key_len][key bytes][uint32 value_len][value bytes]...
// entries. "key" is the FULL internal key (user_key + 8-byte seq/type suffix) so
// the host can feed it directly into an InternalIterator.
struct rocksdb_mvcc_filter_output {
	uint64_t keys_seen;
	uint64_t keys_filtered;
};

struct rocksdb_magic_read_params {
	struct rocksdb_read_params read_params;
	size_t input_buf;
	size_t output_buf;
	size_t output_buf_size;
	size_t file_start_offset[4];
	size_t file_size[4];
	int num_files;
	uint32_t load_task_ids[4];
	int num_load_tasks;
};

struct magic_params {
	struct magic_info info; // filled by CSD on return
	union {
		struct rocksdb_magic_compaction_params compaction;
		struct rocksdb_magic_crc_params crc;
		struct rocksdb_magic_read_params read;
	};
};

struct CSD_PARAMS {
	// unsigned int delay;
	unsigned int finish;
	size_t compressed_size; // used for both compresssion and encoding
	union {
		struct key_finder_params key_finder_params;
		struct key_finder2_params key_finder2_params;
		struct tpch_filter_params tpch_filter_params;
		struct knn_params knn_params;
		struct grep_params grep_params;
		struct bitmap_params bitmap_params;
		struct btree_params btree_params;
		struct compression_params compression_params;
		struct decompression_params decompression_params;
		struct tpch_params tpch_params;
		struct compaction_params compaction_params;
		struct leveldb_compaction_params leveldb_compaction_params;
		struct leveldb_crc_params leveldb_crc_params;
		struct rocksdb_compaction_params rocksdb_compaction_params;
		struct rocksdb_crc_params rocksdb_crc_params;
		struct decoding_params decoding_params;
		struct rocksdb_read_params rocksdb_read_params;
		struct rocksdb_mvcc_filter_params rocksdb_mvcc_filter_params;
	};
	struct profile_info {
		int pid;
		int host_id;
	} profile_info; // used to print real compute time for DCSD
};
// GREP
struct key_finder_format {
	unsigned int data[8];
};

struct tpch_filter_format {
	unsigned int l_orderkey;
	unsigned int l_partkey;
	unsigned int l_suppkey;
	unsigned int l_linenumber;
	size_t l_quantity;
	size_t l_extendedprice;
	int l_shipdate;
	int l_commitdate;
	int l_receiptdate;
	unsigned char l_discount[5];
	unsigned char l_tax[5];
	unsigned char l_returnflag;
	unsigned char l_linestatus;
	unsigned char l_shipinstruct[18];
	unsigned char l_shipmode[10];
	unsigned char l_comment[44];
};

struct compaction_format {
	unsigned char key[16];
	unsigned char value[4080];
};

size_t __csdvirt_run_program(int program_idx, void *buf_in, void *buf_out, size_t size, void *param);
size_t __key_finder(void *buf_in, void *buf_out, size_t size, void *param);
size_t __tpch_filter(void *buf_in, void *buf_out, size_t size, void *param);

// Statistics
struct statistic_format {
	size_t sum;
	unsigned int max;
	unsigned int min;
};

size_t __statistic_int32(void *buf_in, void *buf_out, size_t size, void *param);
size_t __filtered_statistic_int32(void *buf_in, void *buf_out, size_t size, void *param);
size_t __tpch_statistic(void *buf_in, void *buf_out, size_t size, void *param);
size_t __knn(void *buf_in, void *buf_out, size_t size, void *param);
size_t __grep(void *buf_in, void *buf_out, size_t size, void *param);
size_t __bitmap(void *buf_in, void *buf_out, size_t size, void *param);

// Btree
// Node-level information
#define INTERNAL 0
#define LEAF 1
#define BTREE_BLK_SIZE (512)

typedef unsigned char val__t[64];
#define VAL_SIZE sizeof(val__t)

#define NODE_CAPACITY ((BTREE_BLK_SIZE - 2 * sizeof(unsigned long)) / (2 * sizeof(unsigned long)))
#define LOG_CAPACITY (BTREE_BLK_SIZE / VAL_SIZE)

static const size_t FANOUT = NODE_CAPACITY;
struct BtreeNode {
	unsigned long next;
	unsigned long type;
	unsigned long key[NODE_CAPACITY];
	unsigned long ptr[NODE_CAPACITY];
};

struct BtreeLog {
	val__t val[LOG_CAPACITY];
};

struct btree_output {
	size_t key;
	val__t val;
};

size_t __btree(void *buf_in, void *buf_out, size_t size, void *param);
size_t __btree_demand(void *buf_in, void *buf_out, size_t size, void *param);

size_t __compression(void *buf_in, void *buf_out, size_t size, void *param);
size_t __decompression(void *buf_in, void *buf_out, size_t size, void *param);

size_t __quickSort(void *buf_in, void *buf_out, size_t size, void *param);

size_t __compaction(void *buf_in, void *buf_out, size_t size, void *param);

#define FRONT_METADATA_SIZE (16 * 1024)
#define MAX_OUTPUT_TABLES (100)

// TODO: change name to sstable_properties
struct compaction_properties {
	uint64_t raw_key_size;
	uint64_t raw_value_size;
	uint64_t data_size;
	uint64_t index_size;
	uint64_t filter_size;
	uint64_t num_entries;
	uint64_t num_data_blocks;
	uint64_t tail_start_offset;
};

// This struct is kept here since user also use it
struct compacted_file_metadata {
	char smallest_key[32];
	char largest_key[32];
	uint32_t smallest_klen;
	uint32_t largest_klen;
	uint64_t smallest_seqno;
	uint64_t largest_seqno;
	uint64_t tail_size;
	struct compaction_properties properties;
};

struct level_compacted_file_metadata {
	uint64_t datablock_size;
	char smallest_key[32];
	char largest_key[32];
	uint32_t smallest_klen;
	uint32_t largest_klen;
};

struct compaction_stats {
	uint64_t num_input_entries;
	uint64_t raw_input_key_size;
	uint64_t raw_input_value_size;
	uint64_t num_output_entries;
};

struct rocksdb_read_output {
	uint32_t key_length;
	uint32_t value_length;
	uint64_t sequence_number;
};

size_t __leveldb_compaction(void *buf_in, void *buf_out, size_t size, void *param);
size_t __leveldb_crc_calculation(void *buf_in, void *buf_out, size_t size, void *param);
size_t __rocksdb_compaction(void *buf_in, void *buf_out, size_t size, void *param);
size_t __rocksdb_crc_calculation(void *buf_in, void *buf_out, size_t size, void *param);
size_t __rocksdb_read(void *buf_in, void *buf_out, size_t size, void *param);
size_t __rocksdb_mvcc_filter(void *buf_in, void *buf_out, size_t size, void *param);

size_t __magic_rocksdb_compaction(void *param);
size_t __magic_rocksdb_crc_calculation(void *param);
size_t __magic_rocksdb_read(void *param);

size_t __hybrid_encoding(void *buf_in, void *buf_out, size_t size, void *param);

enum TPCHFields {
	tpch_field_orderkey,
	tpch_field_partkey,
	tpch_field_suppkey,
	tpch_field_linenumber,
	tpch_field_quantity,
	tpch_field_extendedprice,
	tpch_field_discount,
	tpch_field_tax,
	tpch_field_returnflag,
	tpch_field_linestatus,
	tpch_field_shipdate,
	tpch_field_commitdate,
	tpch_field_receiptdate,
	tpch_field_shipinstruct,
	tpch_field_shipmode,
	tpch_field_comment,
	tpch_field_count
};

size_t __tpch_filter2(void *buf_in, void *buf_out, size_t size, void *param);

// Utils
void *get_data_from_ptr(size_t ptr, size_t size);
int check_data_using_ptr(size_t ptr, size_t size, int pid, int host_id);

// Init/cleanup for pre-allocated compaction buffers
int csd_user_func_init(void);
void csd_user_func_exit(void);
#endif
