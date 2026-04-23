#ifndef PLUGIN_QUERY_METRICS_VARS_H
#define PLUGIN_QUERY_METRICS_VARS_H

#include <mysql/plugin.h>

#include "my_inttypes.h"

extern bool query_metrics_enabled;
extern bool query_metrics_pfs_enabled;
extern bool query_metrics_warnings_enabled;
extern ulong query_metrics_table_size;
extern ulong query_metrics_sample_rate;
extern bool query_metrics_socket_enabled;
extern char *query_metrics_socket_path;

extern SYS_VAR *query_metrics_system_variables[];
extern SHOW_VAR audit_query_metrics_status[];

void qm_reset_status_counters();
void qm_increment_events_processed();
void qm_increment_events_ignored_wrong_class();
void qm_increment_events_ignored_wrong_subclass();
void qm_increment_events_ignored_wrong_sql_command();
void qm_increment_events_skipped_sampling();
void qm_increment_socket_send_errors();

#endif
