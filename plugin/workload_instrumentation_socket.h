#ifndef PLUGIN_WORKLOAD_INSTRUMENTATION_SOCKET_H
#define PLUGIN_WORKLOAD_INSTRUMENTATION_SOCKET_H

#include <cstddef>

#include "mysql/components/services/registry.h"

int wi_socket_open();
void wi_socket_close();
void wi_socket_send(const char *json, size_t len);

int wi_socket_services_init(SERVICE_TYPE(registry) *reg);
void wi_socket_services_deinit(SERVICE_TYPE(registry) *reg);

#endif  // PLUGIN_WORKLOAD_INSTRUMENTATION_SOCKET_H