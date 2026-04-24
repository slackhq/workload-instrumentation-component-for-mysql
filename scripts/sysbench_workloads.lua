#!/usr/bin/env sysbench

sysbench.cmdline.options = {
  unknown_queries = {"Number of untagged queries per event", 5},
  selects_per_event = {"Number of tagged SELECTs per event", 3},
  inserts_per_event = {"Number of tagged INSERTs per event", 2},
  updates_per_event = {"Number of tagged UPDATEs per event", 2},
  deletes_per_event = {"Number of tagged DELETEs per event", 1},
}

function thread_init()
  drv = sysbench.sql.driver()
  con = drv:connect()
  wname = string.format("workload_%02d", sysbench.tid + 1)
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
      id  INT UNSIGNED NOT NULL AUTO_INCREMENT,
      k   INT UNSIGNED NOT NULL DEFAULT 0,
      c   CHAR(120) NOT NULL DEFAULT '',
      pad CHAR(60)  NOT NULL DEFAULT '',
      PRIMARY KEY (id),
      KEY k_1 (k)
    ) ENGINE=InnoDB
  ]])
  for i = 1, 200 do
    con:query(string.format(
      "INSERT INTO sbtest1 (k, c, pad) VALUES (%d, 'initial-row-%d', 'pad-%d')",
      i, i, i))
  end
  con:disconnect()
end

function cleanup()
  con = sysbench.sql.driver():connect()
  con:query("DROP DATABASE IF EXISTS sbtest")
  con:disconnect()
end

function event()
  local tag = string.format("/* WORKLOAD_NAME=%s */", wname)

  for _ = 1, sysbench.opt.unknown_queries do
    con:query(string.format(
      "SELECT k, c FROM sbtest1 WHERE id = %d",
      sysbench.rand.uniform(1, 200)))
  end

  for _ = 1, sysbench.opt.selects_per_event do
    con:query(string.format(
      "SELECT %s k, c FROM sbtest1 WHERE id = %d",
      tag, sysbench.rand.uniform(1, 200)))
  end

  for _ = 1, sysbench.opt.inserts_per_event do
    con:query(string.format(
      "INSERT %s INTO sbtest1 (k, c, pad) VALUES (%d, 'bench-%s', 'pad')",
      tag, sysbench.rand.uniform(1, 100000), wname))
  end

  for _ = 1, sysbench.opt.updates_per_event do
    con:query(string.format(
      "UPDATE %s sbtest1 SET c = 'updated-%s' WHERE id = %d",
      tag, wname, sysbench.rand.uniform(1, 200)))
  end

  for _ = 1, sysbench.opt.deletes_per_event do
    con:query(string.format(
      "DELETE %s FROM sbtest1 WHERE id = %d",
      tag, sysbench.rand.uniform(201, 999999)))
  end
end