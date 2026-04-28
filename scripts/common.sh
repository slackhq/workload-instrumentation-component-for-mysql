#!/usr/bin/env bash
#
# Shared helpers for test.sh and test_component.sh.
#
# Provides MySQL installation (Oracle & Percona), mysqld initialisation,
# startup, connection wait, plugin-dir detection, and log collection.
#
# Expected to be sourced, not executed directly.  The caller must set
# MYSQL_FLAVOR and MYSQL_VERSION before sourcing; ARTIFACT_DIR and LOG_DIR
# have sensible defaults.
#

MYSQL_FLAVOR="${MYSQL_FLAVOR:?}"
MYSQL_VERSION="${MYSQL_VERSION:?}"
ARTIFACT_DIR="${ARTIFACT_DIR:-artifacts}"
LOG_DIR="${LOG_DIR:-/tmp/test-logs}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MYSQL_DATADIR="/tmp/mysql-data"
MYSQL_SOCK="/tmp/mysql-test.sock"
MYSQL_PORT=13306

export MYSQL_SOCK

mkdir -p "$LOG_DIR"

MYSQL_ERROR_LOG="$MYSQL_DATADIR/error.log"

# Always copy MySQL error log to LOG_DIR on exit (success or failure).
collect_logs() {
  cp "$MYSQL_ERROR_LOG" "$LOG_DIR/" 2>/dev/null || true
  chmod -R a+r "$LOG_DIR" 2>/dev/null || true
  echo "MySQL logs saved to $LOG_DIR"
}
trap collect_logs EXIT

MAJOR_VERSION=$(echo "$MYSQL_VERSION" | sed 's/^\([0-9]*\.[0-9]*\).*/\1/')

# ---- MySQL installation helpers ----

# The apt packages start mysqld automatically; shut it down so we can
# re-initialize with our own datadir and config.
stop_system_mysql() {
  mysql -u root -e "SHUTDOWN" 2>/dev/null || true
  sleep 2
}

# Install a pinned Oracle MySQL version from the official apt repo.
install_oracle_mysql() {
  apt-get update
  apt-get install -y gnupg wget lsb-release curl

  local codename lts_component
  codename=$(lsb_release -cs)

  if [ "$MAJOR_VERSION" = "8.0" ]; then
    lts_component="mysql-8.0"
  else
    lts_component="mysql-${MAJOR_VERSION}-lts"
  fi

  cat > /etc/apt/sources.list.d/mysql.list <<EOSRC
deb [trusted=yes] http://repo.mysql.com/apt/ubuntu/ ${codename} mysql-apt-config
deb [trusted=yes] http://repo.mysql.com/apt/ubuntu/ ${codename} ${lts_component}
deb [trusted=yes] http://repo.mysql.com/apt/ubuntu/ ${codename} mysql-tools
EOSRC

  apt-get update

  local pkg_version
  pkg_version=$(apt-cache madison mysql-server \
    | awk -F'|' -v ver="$MYSQL_VERSION" '$2 ~ ver {gsub(/^ +| +$/,"",$2); print $2; exit}')

  if [ -z "$pkg_version" ]; then
    echo "ERROR: Could not find mysql-server version matching $MYSQL_VERSION" >&2
    echo "Available versions:" >&2
    apt-cache madison mysql-server >&2
    exit 1
  fi

  echo "Installing mysql-server=$pkg_version"
  DEBIAN_FRONTEND=noninteractive apt-get install -y "mysql-server=${pkg_version}" "mysql-client=${pkg_version}"
  stop_system_mysql
}

# Install a pinned Percona Server version from the Percona apt repo.
install_percona_mysql() {
  apt-get update
  apt-get install -y gnupg2 curl lsb-release
  curl -fsSL https://repo.percona.com/apt/percona-release_latest.generic_all.deb -o /tmp/percona-release.deb
  DEBIAN_FRONTEND=noninteractive apt-get install -y /tmp/percona-release.deb

  local ps_series
  ps_series=$(echo "$MAJOR_VERSION" | tr -d '.')
  percona-release setup "ps${ps_series}"
  apt-get update

  local pkg_version
  pkg_version=$(apt-cache madison percona-server-server \
    | awk -F'|' -v ver="$MYSQL_VERSION" '$2 ~ ver {gsub(/^ +| +$/,"",$2); print $2; exit}')

  if [ -z "$pkg_version" ]; then
    echo "ERROR: Could not find percona-server-server version matching $MYSQL_VERSION" >&2
    echo "Available versions:" >&2
    apt-cache madison percona-server-server >&2
    exit 1
  fi

  echo "Installing percona-server-server=$pkg_version"
  DEBIAN_FRONTEND=noninteractive PERCONA_TELEMETRY_DISABLE=1 \
    apt-get install -y --allow-downgrades \
      "percona-server-common=${pkg_version}" \
      "percona-server-client=${pkg_version}" \
      "percona-server-server=${pkg_version}"
  stop_system_mysql
}

# ---- mysqld helpers ----

# Detect the plugin directory from mysqld's built-in default.
detect_plugin_dir() {
  PLUGIN_DIR=$(mysqld --verbose --help 2>&1 | grep '^plugin-dir' | awk '{print $2}' || true)
  if [ -z "$PLUGIN_DIR" ]; then
    PLUGIN_DIR="/usr/lib/mysql/plugin"
  fi
}

# Initialize a fresh datadir, start mysqld in the background, and wait
# for it to accept connections (up to 30 seconds).
start_mysqld() {
  mkdir -p "$MYSQL_DATADIR"
  chown mysql:mysql "$MYSQL_DATADIR"
  mysqld --user=mysql --initialize-insecure --datadir="$MYSQL_DATADIR"

  mysqld --user=mysql \
    --datadir="$MYSQL_DATADIR" \
    --port="$MYSQL_PORT" \
    --socket="$MYSQL_SOCK" \
    --mysqlx=OFF \
    --log-error="$MYSQL_ERROR_LOG" \
    &

  # Wait up to 30 seconds for mysqld to accept connections.
  local ready=0
  for i in $(seq 1 30); do
    if mysql -u root -S "$MYSQL_SOCK" -e "SELECT 1" &>/dev/null; then
      ready=1
      break
    fi
    sleep 1
  done

  if [ "$ready" -eq 0 ]; then
    echo "ERROR: mysqld failed to accept connections within 30 seconds" >&2
    exit 1
  fi

  mysql -u root -S "$MYSQL_SOCK" -e "SELECT VERSION()"
}
