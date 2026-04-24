#ifndef PLUGIN_WORKLOAD_INSTRUMENTATION_WORKLOAD_H
#define PLUGIN_WORKLOAD_INSTRUMENTATION_WORKLOAD_H

#include <mysql/plugin.h>

#include "my_inttypes.h"
#include "mysql/components/services/registry.h"

constexpr size_t WI_WORKLOAD_NAME_BUF_SIZE = 256;

struct query_attrs_t {
  char workload_name[WI_WORKLOAD_NAME_BUF_SIZE];
  ulonglong team_id;
};

query_attrs_t get_query_attrs(MYSQL_THD thd);

int wi_workload_services_init(SERVICE_TYPE(registry) * reg,
                               MYSQL_PLUGIN *plugin_handle);
void wi_workload_services_deinit(SERVICE_TYPE(registry) * reg);

#endif  // PLUGIN_WORKLOAD_INSTRUMENTATION_WORKLOAD_H
