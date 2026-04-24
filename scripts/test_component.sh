#!/usr/bin/env bash
#
# Integration tests for the workload_instrumentation COMPONENT.
#
# Called from test.sh when BUILD_TARGET=component. Installs Oracle MySQL,
# deploys the component .so, creates a test database, and runs the Python
# unittest suite in integration_tests.py.
#
# Required env vars: MYSQL_FLAVOR, MYSQL_VERSION
# Optional env vars: ARTIFACT_DIR (default: artifacts), LOG_DIR (default: /tmp/test-logs)
#
set -euo pipefail

# shellcheck source=scripts/common.sh
source "$(cd "$(dirname "$0")" && pwd)/common.sh"

# ---- Install MySQL and Python ----

case "$MYSQL_FLAVOR" in
  oracle)  install_oracle_mysql ;;
  *)       echo "Unknown MYSQL_FLAVOR for component: $MYSQL_FLAVOR" >&2; exit 1 ;;
esac

DEBIAN_FRONTEND=noninteractive apt-get install -y python3-pip python3-venv

# ---- Deploy component and start mysqld ----

detect_plugin_dir

echo "Copying component to $PLUGIN_DIR"
cp "${ARTIFACT_DIR}/component_workload_instrumentation.so" "$PLUGIN_DIR/"

start_mysqld

# Create the test database and table used by integration_tests.py.
mysql -u root -S "$MYSQL_SOCK" -e "CREATE DATABASE IF NOT EXISTS testdb"
mysql -u root -S "$MYSQL_SOCK" testdb -e '
  CREATE TABLE IF NOT EXISTS test_table (
    id bigint unsigned NOT NULL AUTO_INCREMENT,
    content char(5) DEFAULT NULL,
    PRIMARY KEY (id)
  ) ENGINE=InnoDB
'

# ---- Run Python integration tests ----

python3 -m venv /tmp/tests-venv
. /tmp/tests-venv/bin/activate
pip3 install "mysql-connector-python==${MAJOR_VERSION}"

echo "Running component integration tests..."
python3 -m unittest -v "${SCRIPT_DIR}/integration_tests.py"
