#ifndef PLUGIN_QUERY_METRICS_SOCKET_H
#define PLUGIN_QUERY_METRICS_SOCKET_H

#include <cstddef>

#include "mysql/components/services/registry.h"

int qm_socket_open();
void qm_socket_close();
void qm_socket_send(const char *json, size_t len);

int qm_socket_services_init(SERVICE_TYPE(registry) *reg);
void qm_socket_services_deinit(SERVICE_TYPE(registry) *reg);

#endif