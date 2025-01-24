# Per Workload Instrumentation Component for MySQL (like) Databases

## Introduction
This is a component for MySQL (TM Oracle) to expose basic per workload instrumentation metrics. It gets notifications
from MySQL about query execution completion, parses the workload that ran the query (provided it is specified in a
properly formatted query comment - see below), gathers basic performance information about the execution and exposes the
cumulative per workload metrics in table `performance_schema.workload_instrumentation` table. Particularly, it exposes
the following metrics per workload:

* Workload name (as indicated in query comment).
* Number of queries run.
* Number of rows examined.
* Number of rows returned/sent to the client.
* Number of rows affected.
* Total wallclock duration running queries (in microseconds).

The workload must be identified in a query comment with a comment in the query of the form 
`/* WORKLOAD_NAME=<the workload name> */`. Notice that the workload name must match the regex `[A-Za-z0-9-_:.\/\\\\]+`.
Other than that, the workload can be whatever string you want it to be. Depending on your use case, it could be the name
of an API handler, a job handler, a dev team owning a feature or set of features, etc. Use whatever suits your case.

Queries lacking a workload name comment, or with a workload name that does not match the regex, will be assigned to a
special workload called `__UNSPECIFIED__`.

There's an upper limit of 5k distinct workload names in the component. This means that only the first 5k workload names
seen since the component is loaded are assigned to a special workload `__OVERFLOW__`. Also notice that special workload
names `__UNSPECIFIED__` and `__OVERFLOW__` do not count against the 5k limit.

## Examples
The following examples demonstrate the behavior and functionality of the component:

```sql
user@MySQL> SHOW CREATE TABLE test_table\G
*************************** 1. row ***************************
       Table: test_table
Create Table: CREATE TABLE `test_table` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `content` char(5) DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=15 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci
1 row in set (0.0026 sec)

-- Load the component to start gathering workload stats
  mysql> INSTALL COMPONENT 'file://component_workload_instrumentation';
Query OK, 0 rows affected (0.0357 sec)

-- Run a point SELECT, it should only read & return a single row.
mysql> SELECT /* WORKLOAD_NAME=api_endpoint_1 */ * FROM test_table WHERE id=4;
+----+---------+
| id | content |
+----+---------+
|  4 | dd      |
+----+---------+
1 row in set (0.0011 sec)
Query OK, 0 rows affected (0.0005 sec)

-- Component's performance_schema table confirms indeed 1 query run, 1 row read, 1 row returned.
mysql> SELECT * FROM performance_schema.workload_instrumentation WHERE WORKLOAD='api_endpoint_1';
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
| WORKLOAD       | COUNT_QUERIES | SUM_ROWS_EXAMINED | SUM_ROWS_SENT | SUM_ROWS_AFFECTED | SUM_DURATION_US |
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
| api_endpoint_1 |             1 |                 1 |             1 |                 0 |             459 |
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
1 row in set (0.0073 sec)
Query OK, 0 rows affected (0.0005 sec)

-- Read & return 3 rows by PK
mysql> SELECT /* WORKLOAD_NAME=api_endpoint_1 */ * FROM test_table WHERE id BETWEEN 4 AND 6;
+----+---------+
| id | content |
+----+---------+
|  4 | cc      |
|  5 | cc      |
|  6 | dd      |
+----+---------+
3 rows in set (0.0015 sec)
Query OK, 0 rows affected (0.0006 sec)

-- Plugin table shows 2 queries run and cumulative rows read & returned from previous queries (1 + 3 = 4).
mysql> SELECT * FROM performance_schema.workload_instrumentation WHERE WORKLOAD='api_endpoint_1';
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
| WORKLOAD       | COUNT_QUERIES | SUM_ROWS_EXAMINED | SUM_ROWS_SENT | SUM_ROWS_AFFECTED | SUM_DURATION_US |
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
| api_endpoint_1 |             2 |                 4 |             4 |                 0 |             925 |
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
1 row in set (0.0012 sec)
Query OK, 0 rows affected (0.0005 sec)

-- Full table scan looking for something that doesn't exist.
mysql> SELECT /* WORKLOAD_NAME=api_endpoint_1 */ * FROM test_table WHERE content='ddd';
Empty set (0.0015 sec)
Query OK, 0 rows affected (0.0005 sec)

-- Rows examined shows we went through all the table, yet returned rows did not increase.
mysql> SELECT * FROM performance_schema.workload_instrumentation WHERE WORKLOAD='api_endpoint_1';
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
| WORKLOAD       | COUNT_QUERIES | SUM_ROWS_EXAMINED | SUM_ROWS_SENT | SUM_ROWS_AFFECTED | SUM_DURATION_US |
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
| api_endpoint_1 |             3 |                18 |             4 |                 0 |            1398 |
+----------------+---------------+-------------------+---------------+-------------------+-----------------+
1 row in set (0.0010 sec)
Query OK, 0 rows affected (0.0005 sec)
```

## Why?
MySQL offers outstanding instrumentation grouped by different dimensions under `performance_schema.events_*` tables (
e.g. account, host, instance, user, etc.). Unfortunately, those dimensions do not necessarily match what you would like
to group your metrics by. This component is my attempt at allowing to group by an arbitrary attribute specified in the
query comment, which we call the workload.

## Building
### Requirements
* Supported versions: This has been built & tested against MySQL 8.4 and Percona Server for MySQL 8.4. It does
  not work with version 8.0 due to lack of a required MySQL internal service. 
* You need the full source code of the corresponding MySQL / Percona Server you intend to use the component on. You need
  be able to compile it as provided by the vendor. Do this *before* trying to compile this component. Refer to the
  corresponding documentation for details.

### Compiling
* Simply copy the `workload_instrumentation` directory to the `components` subdirectory of your MySQL server source 
  code.
* Recompile the full MySQL source code. This should compile the component. Alternatively, only compile the `cmake` 
  `component_workload_instrumentation` target. You should end up with file 
  `plugin_output_directory/component_workload_instrumentation.so` under your `cmake` output directory.

### Installation
* Copy `component_workload_instrumentation.so` to where MySQL will find it. This should be the path specified in you
  `plugin_dir` MySQL server variable.
* Run `INSTALL COMPONENT 'file://component_workload_instrumentation';`

