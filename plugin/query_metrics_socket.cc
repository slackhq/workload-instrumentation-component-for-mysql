#include "plugin/query_metrics/query_metrics_socket.h"

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

#include <mysql/plugin.h>

#include "mysql/components/services/component_sys_var_service.h"
#include "plugin/query_metrics/query_metrics.h"
#include "plugin/query_metrics/query_metrics_vars.h"

static std::atomic<int> qm_socket_fd{-1};
static std::atomic<int64_t> qm_socket_last_retry_sec{0};
static const int64_t QM_SOCKET_RETRY_INTERVAL_SEC = 5;

static SERVICE_TYPE(component_sys_variable_register) *qm_sysvar_svc = nullptr;
static my_h_service h_sysvar_svc = nullptr;

static char qm_hostname[256];
static std::atomic<size_t> qm_hostname_len{0};

static const char *const QM_MURRON_TYPE = "mysql_query_metrics";
static const size_t QM_MURRON_TYPE_LEN = 19;
static const int32_t QM_MURRON_VERSION = 1;

static void resolve_hostname() {
  if (qm_sysvar_svc == nullptr) return;

  char buf[256];
  char *val = buf;
  size_t val_len = sizeof(buf) - 1;

  char resolved[256];
  size_t resolved_len = 0;

  if (!qm_sysvar_svc->get_variable("mysql_server", "report_host",
                                    reinterpret_cast<void **>(&val),
                                    &val_len) &&
      val_len > 0) {
    memcpy(resolved, val, val_len);
    resolved[val_len] = '\0';
    resolved_len = val_len;
  } else {
    val = buf;
    val_len = sizeof(buf) - 1;
    if (!qm_sysvar_svc->get_variable("mysql_server", "hostname",
                                      reinterpret_cast<void **>(&val),
                                      &val_len) &&
        val_len > 0) {
      memcpy(resolved, val, val_len);
      resolved[val_len] = '\0';
      resolved_len = val_len;
    } else {
      memcpy(resolved, "unknown", 7);
      resolved[7] = '\0';
      resolved_len = 7;
    }
  }

  memcpy(qm_hostname, resolved, resolved_len + 1);
  std::atomic_thread_fence(std::memory_order_release);
  qm_hostname_len.store(resolved_len, std::memory_order_release);
}

int qm_socket_services_init(SERVICE_TYPE(registry) * reg) {
  my_h_service temp = nullptr;
  if (!reg->acquire("component_sys_variable_register", &temp)) {
    h_sysvar_svc = temp;
    qm_sysvar_svc =
        reinterpret_cast<SERVICE_TYPE(component_sys_variable_register) *>(temp);
  }
  resolve_hostname();
  return 0;
}

void qm_socket_services_deinit(SERVICE_TYPE(registry) * reg) {
  if (h_sysvar_svc != nullptr) {
    reg->release(h_sysvar_svc);
    h_sysvar_svc = nullptr;
    qm_sysvar_svc = nullptr;
  }
}

int qm_socket_open() {
  if (qm_socket_fd.load(std::memory_order_relaxed) >= 0) return 0;

  const char *path = query_metrics_socket_path;
  if (path == nullptr || path[0] == '\0') return 1;

  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (fd < 0) return 1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) <
      0) {
    close(fd);
    return 1;
  }

  int expected = -1;
  if (!qm_socket_fd.compare_exchange_strong(expected, fd,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
    close(fd);
  }
  return 0;
}

void qm_socket_close() {
  int old_fd = qm_socket_fd.exchange(-1, std::memory_order_acq_rel);
  if (old_fd >= 0) {
    close(old_fd);
  }
}

static int64_t murron_timestamp_ns() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000000000LL +
         static_cast<int64_t>(tv.tv_usec) * 1000LL;
}

static void write_le32(char *dst, int32_t v) { memcpy(dst, &v, 4); }

static void write_le64(char *dst, int64_t v) { memcpy(dst, &v, 8); }

static bool qm_socket_try_reconnect() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int64_t now_sec = static_cast<int64_t>(tv.tv_sec);
  int64_t last = qm_socket_last_retry_sec.load(std::memory_order_relaxed);
  if (now_sec - last < QM_SOCKET_RETRY_INTERVAL_SEC) return false;
  if (!qm_socket_last_retry_sec.compare_exchange_strong(
          last, now_sec, std::memory_order_relaxed))
    return false;
  return qm_socket_open() == 0;
}

void qm_socket_send(const char *json, size_t len) {
  int fd = qm_socket_fd.load(std::memory_order_acquire);
  if (fd < 0) {
    if (!qm_socket_try_reconnect()) {
      qm_increment_socket_send_errors();
      return;
    }
    fd = qm_socket_fd.load(std::memory_order_acquire);
    if (fd < 0) {
      qm_increment_socket_send_errors();
      return;
    }
  }

  char host_snap[256];
  size_t host_snap_len = qm_hostname_len.load(std::memory_order_acquire);
  memcpy(host_snap, qm_hostname, host_snap_len);

  size_t frame_size = 4 + 8 + (4 + host_snap_len) + (4 + QM_MURRON_TYPE_LEN) +
                      (4 + len);

  char frame_buf[65536];
  if (frame_size > sizeof(frame_buf)) {
    qm_increment_socket_send_errors();
    return;
  }

  char *p = frame_buf;

  write_le32(p, QM_MURRON_VERSION);
  p += 4;

  write_le64(p, murron_timestamp_ns());
  p += 8;

  write_le32(p, static_cast<int32_t>(host_snap_len));
  p += 4;
  memcpy(p, host_snap, host_snap_len);
  p += host_snap_len;

  write_le32(p, static_cast<int32_t>(QM_MURRON_TYPE_LEN));
  p += 4;
  memcpy(p, QM_MURRON_TYPE, QM_MURRON_TYPE_LEN);
  p += QM_MURRON_TYPE_LEN;

  write_le32(p, static_cast<int32_t>(len));
  p += 4;
  memcpy(p, json, len);

  if (send(fd, frame_buf, frame_size, MSG_DONTWAIT) < 0) {
    qm_increment_socket_send_errors();
    qm_socket_close();
  }
}