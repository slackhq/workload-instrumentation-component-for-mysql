#ifndef PLUGIN_WORKLOAD_INSTRUMENTATION_VARS_H
#define PLUGIN_WORKLOAD_INSTRUMENTATION_VARS_H

#include <mysql/plugin.h>

#include "my_inttypes.h"

extern bool workload_instrumentation_enabled;
extern bool workload_instrumentation_pfs_enabled;
extern bool workload_instrumentation_warnings_enabled;
extern ulong workload_instrumentation_table_size;
extern ulong workload_instrumentation_sample_rate;
extern bool workload_instrumentation_socket_enabled;
extern char *workload_instrumentation_socket_path;

extern SYS_VAR *workload_instrumentation_system_variables[];
extern SHOW_VAR workload_instrumentation_status[];

void wi_reset_status_counters();
void wi_increment_events_processed();
void wi_increment_events_ignored_wrong_class();
void wi_increment_events_ignored_wrong_subclass();
void wi_increment_events_ignored_wrong_sql_command();
void wi_increment_events_skipped_sampling();
void wi_increment_socket_send_errors();

#endif  // PLUGIN_WORKLOAD_INSTRUMENTATION_VARS_H
