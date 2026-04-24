# Workload Instrumentation for MySQL

## Introduction

This project provides per-workload instrumentation for MySQL-compatible databases. It hooks into
query execution completion events, parses the workload name from a specially formatted query comment
or query attribute, gathers performance metrics, and exposes cumulative per-workload statistics in
the `performance_schema.workload_instrumentation` table.

Two implementations are provided:

* **Plugin** (audit API) — for Oracle MySQL 8.0 and Percona Server 8.0. Source in `plugin/`.
* **Component** (service API) — for Oracle MySQL 8.4+. Source in `component/`.

The plugin is currently more feature-complete than the component. Its additional features include:
* The `workload_instrumentation` table has additional per workload statistics.
* In addition to the `performance_schema` table, the plugin also allows to expose per query 
  statistics via two different paths:
  * Back to the client via a MySQL warning.
  * To a unix domain datagram socket from which another process can read them.

We intend to bring the component to feature parity with the plugin in the future.

### Workload identification

The workload name is resolved from two sources, checked in this order:

1. **MySQL query attributes**: A query attribute named `workload_name`. This is the preferred method
   when using connectors that support query attributes (MySQL 8.0.25+).
2. **SQL comment**: A comment of the form `/* WORKLOAD_NAME=<name> */` anywhere in the query text.

The workload name must match the regex `[A-Za-z0-9-_:.\\/\\\\]+`.

Queries that do not carry a valid workload name through either method are assigned to the special
`__UNSPECIFIED__` workload.

#### Team ID (plugin only)

The plugin also extracts an optional team identifier from:

1. A query attribute named `team_id`.
2. A comment tag of the form `|TEAM:<numeric_id>|` inside a SQL comment.

The team ID is included in JSON output (warnings and socket export) but is not stored in the PFS
table.

### Instrumented query types

Only DML statements are instrumented: `SELECT`, `INSERT`, `INSERT...SELECT`, `UPDATE`,
`UPDATE...MULTI`, `DELETE`, `DELETE...MULTI`, `REPLACE`, `REPLACE...SELECT`, and `LOAD DATA`.
All other statement types (DDL, administrative, etc.) are ignored.

## Why?

MySQL offers outstanding instrumentation grouped by different dimensions under
`performance_schema.events_*` tables (e.g. account, host, instance, user, etc.). Unfortunately,
those dimensions do not necessarily match what you would like to group your metrics by. This project
allows grouping by an arbitrary attribute specified in the query comment, which we call the workload.

## Plugin

The plugin uses the MySQL audit API and is compatible with Oracle MySQL 8.0 and Percona Server 8.0.

### Installation

```sql
INSTALL PLUGIN WORKLOAD_INSTRUMENTATION SONAME 'workload_instrumentation.so';
```

### System variables

All variables are dynamic and can be changed at runtime with `SET GLOBAL`.

| Variable | Type | Default | Range | Description |
|---|---|---|---|---|
| `workload_instrumentation_enabled` | bool | `ON` | — | Master switch: disables all instrumentation when OFF |
| `workload_instrumentation_pfs_enabled` | bool | `ON` | — | Expose metrics in the `performance_schema` table. PFS services are lazy-initialized on first enable |
| `workload_instrumentation_warnings_enabled` | bool | `ON` | — | Push a JSON note-level warning with per-query metrics to the client after each instrumented query |
| `workload_instrumentation_table_size` | ulong | `10000` | 1–1,000,000 | Maximum number of distinct workload slots. Changing this at runtime reallocates the table and copies existing data |
| `workload_instrumentation_sample_rate` | ulong | `100` | 1–100 | Percentage of queries to instrument. Uses per-thread random sampling |
| `workload_instrumentation_socket_enabled` | bool | `OFF` | — | Send per-query JSON metrics to a Unix DGRAM socket |
| `workload_instrumentation_socket_path` | string | `NULL` | — | Path to the Unix DGRAM socket. Changing this at runtime reconnects automatically |

### Status variables

Available via `SHOW GLOBAL STATUS LIKE 'workload_instrumentation%'`.

| Status | Description |
|---|---|
| `workload_instrumentation_events_processed` | Total events received by the plugin |
| `workload_instrumentation_events_ignored_wrong_class` | Events rejected: not a query event |
| `workload_instrumentation_events_ignored_wrong_subclass` | Events rejected: not a query completion event |
| `workload_instrumentation_events_ignored_wrong_sql_command` | Events rejected: not an instrumented DML type |
| `workload_instrumentation_events_skipped_sampling` | Events skipped due to sampling rate |
| `workload_instrumentation_socket_send_errors` | Failed socket sends (triggers auto-reconnect after 5 seconds) |

### PFS table schema

The `performance_schema.workload_instrumentation` table supports `SELECT` and `TRUNCATE TABLE`.
It has a `WORKLOAD_NAME VARCHAR(256)` column (indexed) and the following metric columns:

| Column | Description |
|---|---|
| `COUNT_STAR` | Number of queries |
| `SUM_TIMER_WAIT` | Total wallclock duration (picoseconds) |
| `SUM_CPU_TIME` | Total CPU time (picoseconds) |
| `SUM_LOCK_TIME` | Total lock wait time (picoseconds) |
| `SUM_ROWS_AFFECTED` | Rows affected (INSERT/UPDATE/DELETE) |
| `SUM_ROWS_EXAMINED` | Rows examined |
| `SUM_ROWS_SENT` | Rows returned to client |
| `SUM_CREATED_TMP_DISK_TABLES` | On-disk temp tables created |
| `SUM_CREATED_TMP_TABLES` | In-memory temp tables created |
| `SUM_SELECT_FULL_JOIN` | Joins without index |
| `SUM_SELECT_FULL_RANGE_JOIN` | Joins using range on reference table |
| `SUM_SELECT_RANGE` | Joins using range on first table |
| `SUM_SELECT_RANGE_CHECK` | Joins re-checking keys per row |
| `SUM_SELECT_SCAN` | Full table scans on first table |
| `SUM_SORT_MERGE_PASSES` | Sort merge passes |
| `SUM_SORT_RANGE` | Sorts using ranges |
| `SUM_SORT_ROWS` | Rows sorted |
| `SUM_SORT_SCAN` | Sorts using full scans |
| `SUM_NO_INDEX_USED` | Queries with no index |
| `SUM_NO_GOOD_INDEX_USED` | Queries with no good index |

### Special workloads

* `__UNSPECIFIED__` — queries without a valid workload name.
* `__OVERFLOW__` — queries that arrived after the table was full.

### Per-query JSON output

When `warnings_enabled` or `socket_enabled` is ON, the plugin produces a JSON object for each
instrumented query containing: `workload_name`, `team_id`, `timer_wait_ns`, `cpu_time_ns`,
`lock_time_us`, `rows_affected`, `rows_examined`, `rows_sent`, `created_tmp_disk_tables`,
`created_tmp_tables`, `select_full_join`, `select_full_range_join`, `select_range`,
`select_range_check`, `select_scan`, `sort_merge_passes`, `sort_range`, `sort_rows`, `sort_scan`,
`no_index_used`, `no_good_index_used`.

With `warnings_enabled = ON`, this JSON is pushed to the client as a note-level warning.

With `socket_enabled = ON`, the JSON is sent over a Unix DGRAM socket wrapped in a binary Murron
frame (version, nanosecond timestamp, hostname, message type, payload). The socket auto-reconnects
every 5 seconds on failure. The hostname is resolved from `report_host` or `hostname` server
variables.

### Example

```sql
mysql> INSTALL PLUGIN WORKLOAD_INSTRUMENTATION SONAME 'workload_instrumentation.so';

mysql> SELECT /* WORKLOAD_NAME=api_endpoint_1 */ * FROM test_table WHERE id=4;
+----+---------+
| id | content |
+----+---------+
|  4 | dd      |
+----+---------+

mysql> SELECT * FROM performance_schema.workload_instrumentation WHERE WORKLOAD_NAME='api_endpoint_1';
+----------------+------------+----------------+--------------+---------------+-------------------+-------------------+---------------+-----------------------------+-------------------------+----------------------+----------------------------+------------------+------------------------+------------------+----------------------+----------------+---------------+---------------+---------------------+--------------------------+
| WORKLOAD_NAME  | COUNT_STAR | SUM_TIMER_WAIT | SUM_CPU_TIME | SUM_LOCK_TIME | SUM_ROWS_AFFECTED | SUM_ROWS_EXAMINED | SUM_ROWS_SENT | SUM_CREATED_TMP_DISK_TABLES | SUM_CREATED_TMP_TABLES | SUM_SELECT_FULL_JOIN | SUM_SELECT_FULL_RANGE_JOIN | SUM_SELECT_RANGE | SUM_SELECT_RANGE_CHECK | SUM_SELECT_SCAN | SUM_SORT_MERGE_PASSES | SUM_SORT_RANGE | SUM_SORT_ROWS | SUM_SORT_SCAN | SUM_NO_INDEX_USED | SUM_NO_GOOD_INDEX_USED |
+----------------+------------+----------------+--------------+---------------+-------------------+-------------------+---------------+-----------------------------+-------------------------+----------------------+----------------------------+------------------+------------------------+------------------+----------------------+----------------+---------------+---------------+---------------------+--------------------------+
| api_endpoint_1 |          1 |      459567000 |      6712000 |             0 |                 0 |                 1 |             1 |                           0 |                       0 |                    0 |                          0 |                0 |                      0 |                0 |                    0 |              0 |             0 |             0 |                   0 |                        0 |
+----------------+------------+----------------+--------------+---------------+-------------------+-------------------+---------------+-----------------------------+-------------------------+----------------------+----------------------------+------------------+------------------------+------------------+----------------------+----------------+---------------+---------------+---------------------+--------------------------+
```

Columns are consistent with and in the same units as other `performance_schema` columns, including times in `ps`.

## Component

The component uses the MySQL service API and is compatible with Oracle MySQL 8.4+.

### Installation

```sql
INSTALL COMPONENT 'file://component_workload_instrumentation';
```

### PFS table schema

The `performance_schema.workload_instrumentation` table has a `WORKLOAD VARCHAR(50)` column and
the following metric columns:

| Column | Description |
|---|---|
| `COUNT_QUERIES` | Number of queries |
| `SUM_ROWS_EXAMINED` | Rows examined |
| `SUM_ROWS_SENT` | Rows returned to client |
| `SUM_ROWS_AFFECTED` | Rows affected (INSERT/UPDATE/DELETE) |
| `SUM_DURATION_US` | Total wallclock duration (microseconds) |

### Special workloads

* `__UNSPECIFIED__` — queries without a valid workload name.
* `__OVERFLOW__` — queries that arrived after the table was full.

The table has a fixed capacity of 5,000 workload slots (plus `__UNSPECIFIED__` and `__OVERFLOW__`).

### Example

```sql
mysql> INSTALL COMPONENT 'file://component_workload_instrumentation';

mysql> SELECT /* WORKLOAD_NAME=api_endpoint_1 */ * FROM test_table WHERE id=4;
+----+---------+
| id | content |
+----+---------+
|  4 | dd      |
+----+---------+

mysql> SELECT * FROM performance_schema.workload_instrumentation WHERE WORKLOAD='api_endpoint_1';
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
| WORKLOAD       | COUNT_QUERIES | SUM_ROWS_EXAMINED | SUM_ROWS_SENT | SUM_ROWS_AFFECTED | SUM_DURATION_US |
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
| api_endpoint_1 |             1 |                 1 |             1 |                 0 |             459 |
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
```

## Building

### Requirements

* **Plugin**: Oracle MySQL 8.0 or Percona Server for MySQL 8.0 source code.
* **Component**: Oracle MySQL 8.4+ source code.
* You need the full source code of the corresponding MySQL / Percona Server you intend to build
  against. You must be able to compile it as provided by the vendor before attempting to build this
  project. Refer to the corresponding vendor documentation for details.

### Using Make (recommended)

The Makefile wraps the build and test workflow in Docker containers:

```bash
# Build the plugin for Oracle MySQL 8.0.46
make build BUILD_TARGET=plugin MYSQL_FLAVOR=oracle MYSQL_VERSION=8.0.46 UBUNTU_VERSION=22.04

# Build the component for Oracle MySQL 8.4.9
make build BUILD_TARGET=component MYSQL_FLAVOR=oracle MYSQL_VERSION=8.4.9 UBUNTU_VERSION=24.04

# Build the plugin for Percona Server 8.0.36-28
make build BUILD_TARGET=plugin MYSQL_FLAVOR=percona MYSQL_VERSION=8.0.36-28 UBUNTU_VERSION=22.04
```

Artifacts are placed in `dist/<target>/<flavor>-<version>/`.

### Manual compilation

* Copy the `plugin/` directory to `<mysql-src>/plugin/workload_instrumentation`, or the
  `component/` directory to `<mysql-src>/components/workload_instrumentation`.
* Run cmake and build the corresponding target:
  * Plugin: `make workload_instrumentation`
  * Component: `make component_workload_instrumentation`
* The `.so` file will be in `plugin_output_directory/` under your cmake build directory.

## Testing

Integration tests run inside Docker containers:

```bash
# Test plugin on Oracle MySQL 8.0.46
make test BUILD_TARGET=plugin MYSQL_FLAVOR=oracle MYSQL_VERSION=8.0.46 UBUNTU_VERSION=22.04

# Test plugin on Percona Server 8.0.36-28
make test BUILD_TARGET=plugin MYSQL_FLAVOR=percona MYSQL_VERSION=8.0.36-28 UBUNTU_VERSION=22.04

# Test component on Oracle MySQL 8.4.9
make test BUILD_TARGET=component MYSQL_FLAVOR=oracle MYSQL_VERSION=8.4.9 UBUNTU_VERSION=24.04
```

Plugin tests use sysbench workloads with deterministic verification of all PFS counters.
Component tests use a Python unittest suite that verifies counter accuracy, table capacity, and
overflow/unspecified behavior.
