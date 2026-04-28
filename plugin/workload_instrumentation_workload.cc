#include "plugin/workload_instrumentation/workload_instrumentation_workload.h"

#include <cstdlib>
#include <cstring>

#include <mysql/plugin.h>

#include "mysql/components/my_service.h"
#include "mysql/components/services/mysql_query_attributes.h"
#include "mysql/components/services/mysql_string.h"
#include "mysql/components/services/registry.h"
#include "sql/sql_class.h"

#ifndef _GNU_SOURCE
static void *memmem(const void *haystack, size_t haystack_len,
                    const void *needle, size_t needle_len) {
  if (needle_len == 0) return const_cast<void *>(haystack);
  if (needle_len > haystack_len) return nullptr;
  const char *h = static_cast<const char *>(haystack);
  const char *n = static_cast<const char *>(needle);
  const char *end = h + haystack_len - needle_len;
  for (; h <= end; h++) {
    if (memcmp(h, n, needle_len) == 0)
      return const_cast<void *>(static_cast<const void *>(h));
  }
  return nullptr;
}
#endif

static SERVICE_TYPE(
    mysql_query_attributes_iterator) *query_attrs_iterator_service = nullptr;
static SERVICE_TYPE(mysql_query_attribute_string) *query_attr_string_service =
    nullptr;
static SERVICE_TYPE(mysql_string_factory) *string_factory_service = nullptr;
static SERVICE_TYPE(mysql_string_converter) *string_converter_service = nullptr;

static my_h_service h_query_attrs_iterator_svc = nullptr;
static my_h_service h_query_attr_string_svc = nullptr;
static my_h_service h_string_factory_svc = nullptr;
static my_h_service h_string_converter_svc = nullptr;

static const char *read_query_attribute_string(MYSQL_THD thd,
                                               const char *attr_name,
                                               char *buf, size_t buf_size) {
  if (query_attrs_iterator_service == nullptr ||
      query_attr_string_service == nullptr ||
      string_converter_service == nullptr) {
    return nullptr;
  }

  mysqlh_query_attributes_iterator iterator = nullptr;
  if (query_attrs_iterator_service->create(thd, attr_name, &iterator))
    return nullptr;

  const char *result = nullptr;
  my_h_string str_handle = nullptr;
  if (!query_attr_string_service->get(iterator, &str_handle)) {
    if (!string_converter_service->convert_to_buffer(str_handle, buf,
                                                     buf_size - 1, "utf8mb4")) {
      buf[buf_size - 1] = '\0';
      result = buf;
    }
    if (string_factory_service != nullptr)
      string_factory_service->destroy(str_handle);
  }

  query_attrs_iterator_service->release(iterator);
  return result;
}

static void get_query_attrs_from_query_attributes(MYSQL_THD thd,
                                                   query_attrs_t &attrs) {
  const char *wl = read_query_attribute_string(thd, "workload_name",
                                               attrs.workload_name,
                                               WI_WORKLOAD_NAME_BUF_SIZE);
  if (wl == nullptr) attrs.workload_name[0] = '\0';

  char team_buf[32];
  const char *tid = read_query_attribute_string(thd, "team_id",
                                                team_buf, sizeof(team_buf));
  if (tid != nullptr && tid[0] != '\0') attrs.team_id = strtoull(tid, nullptr, 10);
}

static inline bool is_workload_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':' ||
         c == '.' || c == '/' || c == '\\';
}

static const char *const WORKLOAD_KEY = "WORKLOAD_NAME=";
static const size_t WORKLOAD_KEY_LEN = 14;

static const char *const TEAM_KEY = "|TEAM:";
static const size_t TEAM_KEY_LEN = 6;

static void get_query_attrs_from_query_comment(const char *query,
                                                query_attrs_t &attrs,
                                                bool need_workload,
                                                bool need_team) {
  const char *p = query;
  while ((p = strstr(p, "/*")) != nullptr) {
    p += 2;
    const char *comment_end = strstr(p, "*/");
    if (comment_end == nullptr) break;

    if (need_workload) {
      const char *key = p;
      while (key < comment_end) {
        key = static_cast<const char *>(
            memmem(key, comment_end - key, WORKLOAD_KEY, WORKLOAD_KEY_LEN));
        if (key == nullptr) break;

        const char *val_start = key + WORKLOAD_KEY_LEN;
        const char *val_end = val_start;
        while (val_end < comment_end && is_workload_char(*val_end)) val_end++;

        size_t len = val_end - val_start;
        if (len > 0) {
          if (len >= WI_WORKLOAD_NAME_BUF_SIZE)
            len = WI_WORKLOAD_NAME_BUF_SIZE - 1;
          memcpy(attrs.workload_name, val_start, len);
          attrs.workload_name[len] = '\0';
          need_workload = false;
          break;
        }
        key = val_end;
      }
    }

    if (need_team) {
      const char *tk = static_cast<const char *>(
          memmem(p, comment_end - p, TEAM_KEY, TEAM_KEY_LEN));
      if (tk != nullptr) {
        const char *digit_start = tk + TEAM_KEY_LEN;
        if (digit_start < comment_end && *digit_start >= '0' &&
            *digit_start <= '9') {
          char *end_ptr = nullptr;
          ulonglong val = strtoull(digit_start, &end_ptr, 10);
          if (end_ptr != nullptr && end_ptr <= comment_end &&
              *end_ptr == '|') {
            attrs.team_id = val;
            need_team = false;
          }
        }
      }
    }

    if (!need_workload && !need_team) break;

    p = comment_end + 2;
  }
}

query_attrs_t get_query_attrs(MYSQL_THD thd) {
  query_attrs_t attrs;
  attrs.workload_name[0] = '\0';
  attrs.team_id = 0;

  get_query_attrs_from_query_attributes(thd, attrs);

  bool need_workload = (attrs.workload_name[0] == '\0');
  bool need_team = (attrs.team_id == 0);

  if (need_workload || need_team) {
    get_query_attrs_from_query_comment(thd->query().str, attrs,
                                       need_workload, need_team);
  }

  if (attrs.workload_name[0] == '\0') {
    memcpy(attrs.workload_name, "__UNSPECIFIED__", 16);
  }

  return attrs;
}

int wi_workload_services_init(SERVICE_TYPE(registry) * reg,
                               MYSQL_PLUGIN *plugin_handle) {
  my_h_service temp_service = nullptr;

  if (!reg->acquire("mysql_query_attributes_iterator", &temp_service)) {
    h_query_attrs_iterator_svc = temp_service;
    query_attrs_iterator_service =
        reinterpret_cast<SERVICE_TYPE(mysql_query_attributes_iterator) *>(
            temp_service);
  } else {
    my_plugin_log_message(
        plugin_handle, MY_WARNING_LEVEL,
        "Failed to acquire query attributes iterator service");
  }

  temp_service = nullptr;
  if (!reg->acquire("mysql_query_attribute_string", &temp_service)) {
    h_query_attr_string_svc = temp_service;
    query_attr_string_service =
        reinterpret_cast<SERVICE_TYPE(mysql_query_attribute_string) *>(
            temp_service);
  } else {
    my_plugin_log_message(plugin_handle, MY_WARNING_LEVEL,
                          "Failed to acquire query attribute string service");
  }

  temp_service = nullptr;
  if (!reg->acquire("mysql_string_factory", &temp_service)) {
    h_string_factory_svc = temp_service;
    string_factory_service =
        reinterpret_cast<SERVICE_TYPE(mysql_string_factory) *>(temp_service);
  } else {
    my_plugin_log_message(plugin_handle, MY_WARNING_LEVEL,
                          "Failed to acquire string factory service");
  }

  temp_service = nullptr;
  if (!reg->acquire("mysql_string_converter", &temp_service)) {
    h_string_converter_svc = temp_service;
    string_converter_service =
        reinterpret_cast<SERVICE_TYPE(mysql_string_converter) *>(temp_service);
  } else {
    my_plugin_log_message(plugin_handle, MY_WARNING_LEVEL,
                          "Failed to acquire string converter service");
  }

  return 0;
}

void wi_workload_services_deinit(SERVICE_TYPE(registry) * reg) {
  if (h_query_attrs_iterator_svc != nullptr) {
    reg->release(h_query_attrs_iterator_svc);
    h_query_attrs_iterator_svc = nullptr;
    query_attrs_iterator_service = nullptr;
  }
  if (h_query_attr_string_svc != nullptr) {
    reg->release(h_query_attr_string_svc);
    h_query_attr_string_svc = nullptr;
    query_attr_string_service = nullptr;
  }
  if (h_string_factory_svc != nullptr) {
    reg->release(h_string_factory_svc);
    h_string_factory_svc = nullptr;
    string_factory_service = nullptr;
  }
  if (h_string_converter_svc != nullptr) {
    reg->release(h_string_converter_svc);
    h_string_converter_svc = nullptr;
    string_converter_service = nullptr;
  }
}
