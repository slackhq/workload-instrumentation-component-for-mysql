#ifndef PLUGIN_WORKLOAD_INSTRUMENTATION_TABLE_H
#define PLUGIN_WORKLOAD_INSTRUMENTATION_TABLE_H

#include <mysql/components/service_implementation.h>
#include <mysql/components/services/pfs_plugin_table_service.h>
#include <mysql/plugin.h>

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>

#include "my_inttypes.h"
#include "mysql/psi/mysql_rwlock.h"

/* Maximum number of characters in a workload name. */
constexpr unsigned int WI_WORKLOAD_NAME_LEN = 256;

/*
  Maximum byte length for a workload name in utf8mb4 encoding.
  Each character can be up to 4 bytes, so 256 chars * 4 = 1024 bytes.
*/
constexpr unsigned int WI_WORKLOAD_NAME_CHARSET_LEN = WI_WORKLOAD_NAME_LEN * 4;

/*
  PFS service handles, defined in workload_instrumentation.cc.

  - wi_col_bigint_svc:  Used by wi_read_column_value() to write BIGINT
                         UNSIGNED column values into PFS fields.
  - wi_col_string_svc:  Used by wi_read_column_value() to write VARCHAR
                         column values into PFS fields.
  - wi_table_svc:       Used during plugin init/deinit to register and
                         unregister the performance_schema.workload_instrumentation table
                         via add_tables()/delete_tables().
*/
#include "mysql/components/services/registry.h"

extern SERVICE_TYPE(pfs_plugin_column_bigint_v1) *wi_col_bigint_svc;
extern SERVICE_TYPE(pfs_plugin_column_string_v2) *wi_col_string_svc;
extern SERVICE_TYPE(pfs_plugin_table_v1) *wi_table_svc;

/*
  PFS table share structures.

  - wi_st_share:       The table share proxy that holds the table name,
                        column definitions, access control, and all callback
                        function pointers. Initialized via init_wi_share().
  - wi_share_list:     Array of share pointers passed to add_tables()/
                        delete_tables(). Contains a single entry for our table.
  - wi_share_list_count: Number of entries in wi_share_list (always 1).
*/
extern PFS_engine_table_share_proxy wi_st_share;
extern PFS_engine_table_share_proxy *wi_share_list[1];
extern unsigned int wi_share_list_count;

/*
  User-configurable system variable (workload_instrumentation.table_size) that controls
  the maximum number of distinct workload_name rows the table can hold.
  Defined in workload_instrumentation_vars.cc.
*/
extern ulong workload_instrumentation_table_size;

extern std::unordered_map<std::string, unsigned int> wi_workload_map;
extern mysql_rwlock_t wi_rwlock;

/*
  WI_Record: The live, shared row stored in the global wi_records_array.

  Each slot in the array represents one distinct workload_name. All counter
  fields are std::atomic so that the audit notify callback (which runs on
  arbitrary session threads concurrently) can perform lock-free fetch_add()
  without requiring a mutex.

  m_exist is an atomic<bool> used as a "slot occupied" flag. It is set to
  true under the write lock after claiming a free slot in
  wi_find_or_create_record_wlocked().
*/
struct WI_Record {
  char workload_name[WI_WORKLOAD_NAME_CHARSET_LEN];
  unsigned int workload_name_length;

  std::atomic<ulonglong> count_star{0};
  std::atomic<ulonglong> sum_timer_wait{0};
  std::atomic<ulonglong> sum_cpu_time{0};
  std::atomic<ulonglong> sum_lock_time{0};
  std::atomic<ulonglong> sum_rows_affected{0};
  std::atomic<ulonglong> sum_rows_examined{0};
  std::atomic<ulonglong> sum_rows_sent{0};

  std::atomic<ulonglong> sum_created_tmp_disk_tables{0};
  std::atomic<ulonglong> sum_created_tmp_tables{0};
  std::atomic<ulonglong> sum_select_full_join{0};
  std::atomic<ulonglong> sum_select_full_range_join{0};
  std::atomic<ulonglong> sum_select_range{0};
  std::atomic<ulonglong> sum_select_range_check{0};
  std::atomic<ulonglong> sum_select_scan{0};
  std::atomic<ulonglong> sum_sort_merge_passes{0};
  std::atomic<ulonglong> sum_sort_range{0};
  std::atomic<ulonglong> sum_sort_rows{0};
  std::atomic<ulonglong> sum_sort_scan{0};
  std::atomic<ulonglong> sum_no_index_used{0};
  std::atomic<ulonglong> sum_no_good_index_used{0};

  std::atomic<bool> m_exist{false};
};

/*
  WI_Row_Copy: A point-in-time snapshot of a WI_Record, used by the PFS
  table scan cursor.

  When PFS reads the table (SELECT), wi_rnd_next() copies the atomic values
  from a WI_Record into this plain (non-atomic) struct. This provides a
  consistent snapshot of each row for the duration of the read, avoiding
  tearing that could occur if we read atomics directly across multiple
  wi_read_column_value() calls for the same row.

  The PSI_ubigint type is the PFS service struct {unsigned long long val;
  bool is_null;} used to pass values to the column bigint service.
*/
struct WI_Row_Copy {
  char workload_name[WI_WORKLOAD_NAME_CHARSET_LEN];
  unsigned int workload_name_length;

  PSI_ubigint count_star;
  PSI_ubigint sum_timer_wait;
  PSI_ubigint sum_cpu_time;
  PSI_ubigint sum_lock_time;
  PSI_ubigint sum_rows_affected;
  PSI_ubigint sum_rows_examined;
  PSI_ubigint sum_rows_sent;

  PSI_ubigint sum_created_tmp_disk_tables;
  PSI_ubigint sum_created_tmp_tables;
  PSI_ubigint sum_select_full_join;
  PSI_ubigint sum_select_full_range_join;
  PSI_ubigint sum_select_range;
  PSI_ubigint sum_select_range_check;
  PSI_ubigint sum_select_scan;
  PSI_ubigint sum_sort_merge_passes;
  PSI_ubigint sum_sort_range;
  PSI_ubigint sum_sort_rows;
  PSI_ubigint sum_sort_scan;
  PSI_ubigint sum_no_index_used;
  PSI_ubigint sum_no_good_index_used;
};

/* Global array of WI_Record slots, heap-allocated with workload_instrumentation_table_size
   entries. */
extern WI_Record *wi_records_array;

/*
  WI_POS: Index-based cursor position for iterating over wi_records_array.

  PFS uses a two-position pattern:
    - m_pos:      The current row being read.
    - m_next_pos: Where to resume on the next rnd_next() call.

  has_more() is implemented in the .cc file because it reads the extern
  workload_instrumentation_table_size to determine the array boundary.
*/
class WI_POS {
  unsigned int m_index = 0;

 public:
  bool has_more();
  void next() { m_index++; }
  void reset() { m_index = 0; }
  unsigned int get_index() const { return m_index; }
  void set_at(unsigned int index) { m_index = index; }
  void set_at(WI_POS *pos) { m_index = pos->m_index; }
  void set_after(WI_POS *pos) { m_index = pos->m_index + 1; }
};

/*
  WI_index_by_workload_name: PFS index on the WORKLOAD_NAME column.

  Used by index_init/index_read/index_next to support indexed lookups
  like: SELECT * FROM workload_instrumentation WHERE WORKLOAD_NAME = 'foo';
*/
class WI_index_by_workload_name {
 public:
  PSI_plugin_key_string m_workload_name;
  char m_workload_name_buffer[WI_WORKLOAD_NAME_CHARSET_LEN];

  bool match(WI_Record *record) {
    return wi_col_string_svc->match_key_string(
        false, record->workload_name, record->workload_name_length,
        &m_workload_name);
  }
};

/*
  WI_Table_Handle: Per-SELECT state allocated in wi_open_table() and freed
  in wi_close_table().

  Holds the two cursor positions, the WI_Row_Copy snapshot for the row
  currently being read by wi_read_column_value(), and index state.
*/
struct WI_Table_Handle {
  WI_POS m_pos;
  WI_POS m_next_pos;
  WI_Row_Copy current_row;

  WI_index_by_workload_name m_workload_name_index;
  unsigned int index_num;
};

/* PFS table lifecycle callbacks. */
PSI_table_handle *wi_open_table(PSI_pos **pos);  /* Allocates WI_Table_Handle */
void wi_close_table(PSI_table_handle *handle);   /* Frees WI_Table_Handle */

/* Sequential scan callbacks. */
int wi_rnd_next(PSI_table_handle *handle);  /* Advance to next existing row */
int wi_rnd_init(PSI_table_handle *h, bool scan);  /* No-op, scan setup */
int wi_rnd_pos(PSI_table_handle *handle);   /* Read row at current position */

/* Index scan callbacks. */
int wi_index_init(PSI_table_handle *handle, uint idx, bool sorted,
                   PSI_index_handle **index);
int wi_index_read(PSI_index_handle *index, PSI_key_reader *reader,
                   unsigned int idx, int find_flag);
int wi_index_next(PSI_table_handle *handle);

/* Reset both m_pos and m_next_pos to the beginning. */
void wi_reset_position(PSI_table_handle *handle);

/*
  Column read callback. Maps column index (0..20) to the appropriate PFS
  service call. See init_wi_share() for the full column list.
*/
int wi_read_column_value(PSI_table_handle *handle, PSI_field *field,
                          uint index);

/* Implements TRUNCATE TABLE: resets all slots to empty and zeroes counters. */
int wi_delete_all_rows(void);

/* Returns the current number of occupied slots. */
unsigned long long wi_get_row_count(void);

/*
  Populates the PFS_engine_table_share_proxy with table name, column
  definitions, access control (TRUNCATABLE), and all callback function
  pointers. Called once during plugin init.
*/
void init_wi_share(PFS_engine_table_share_proxy *share);

/* Allocate/free the wi_records_array based on workload_instrumentation_table_size. */
void wi_allocate_records();
void wi_free_records();

unsigned int wi_get_rows_in_table();
void wi_resize_table(ulong new_size);

class query_stats_t;
void wi_aggregate_stats(const char *workload_name,
                         const query_stats_t &stats, ulonglong wall_time_ps,
                         ulonglong cpu_time_ps, ulonglong lock_time_ps);

bool wi_pfs_services_initialized();

int wi_pfs_services_init(SERVICE_TYPE(registry) * reg,
                          MYSQL_PLUGIN *plugin_handle);
void wi_pfs_services_deinit(SERVICE_TYPE(registry) * reg);
int wi_pfs_check_uninstall();

#endif  // PLUGIN_WORKLOAD_INSTRUMENTATION_TABLE_H