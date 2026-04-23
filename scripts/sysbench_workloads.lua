#!/usr/bin/env sysbench

function thread_init()
  drv = sysbench.sql.driver()
  con = drv:connect()
end

function thread_done()
  con:disconnect()
end

function prepare()
  con = sysbench.sql.driver():connect()
  con:query("CREATE DATABASE IF NOT EXISTS sbtest")
  con:query("USE sbtest")
  con:query([[
    CREATE TABLE IF NOT EXISTS sbtest1 (
      id INT UNSIGNED NOT NULL AUTO_INCREMENT,
      k  INT UNSIGNED NOT NULL DEFAULT 0,
      c  CHAR(120) NOT NULL DEFAULT '',
      PRIMARY KEY (id)
    ) ENGINE=InnoDB
  ]])
  for i = 1, 100 do
    con:query(string.format(
      "INSERT INTO sbtest1 (k, c) VALUES (%d, 'row-%d')", i, i))
  end
  con:disconnect()
end

function cleanup()
  con = sysbench.sql.driver():connect()
  con:query("DROP DATABASE IF EXISTS sbtest")
  con:disconnect()
end

local queries_per_workload = 10
local num_unknown = 20

function event()
  for q = 1, num_unknown do
    con:query(string.format(
      "SELECT * FROM sbtest1 WHERE id = %d", q % 100 + 1))
  end

  for w = 1, 15 do
    local wname = string.format("workload_%02d", w)
    for q = 1, queries_per_workload do
      con:query(string.format(
        "SELECT /* WORKLOAD_NAME=%s */ * FROM sbtest1 WHERE id = %d",
        wname, (w * queries_per_workload + q) % 100 + 1))
    end
  end
end
