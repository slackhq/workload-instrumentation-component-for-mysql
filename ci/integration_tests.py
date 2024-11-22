import unittest
import mysql.connector


class IntegrationTest(unittest.TestCase):

    def manage_component(self, action):
        cursor = self.cnx.cursor()
        CMD="INSTALL" if action else "UNINSTALL"
        cursor.execute(f"{CMD} COMPONENT 'file://component_workload_instrumentation'")
        cursor.close()

    def setUp(self):
        self.cnx = mysql.connector.connect(user='root', unix_socket='/tmp/data/mysql.sock', database='testdb')
        cursor = self.cnx.cursor()
        cursor.execute("TRUNCATE TABLE test_table")
        cursor.close()
        self.manage_component(True)
        self.create_test_data()

    def tearDown(self):
        self.manage_component(False)
        self.cnx.close()

    def create_test_data(self):
        for i in range(1, 15):
            cursor = self.cnx.cursor()
            cursor.execute("INSERT /* WORKLOAD_NAME=batch_job_1 */ INTO test_table (content) VALUES ('cc')")
            cursor.close()
        cursor = self.cnx.cursor()
        cursor.execute("UPDATE /* WORKLOAD_NAME=batch_job_1 */ test_table SET content='dd' WHERE id BETWEEN 6 AND 9")
        cursor.close()

    def test_counters_match(self):
        queries = [
            "SELECT /* WORKLOAD_NAME=api_endpoint_1 */ * FROM test_table WHERE id=4",
            "SELECT /* WORKLOAD_NAME=api_endpoint_1 */ * FROM test_table WHERE id BETWEEN 4 AND 6;",
            "SELECT /* WORKLOAD_NAME=api_endpoint_1 */ * FROM test_table WHERE content='ddd';",
        ]

        for query in queries:
            cursor = self.cnx.cursor()
            cursor.execute(query)
            for _ in cursor:
                pass
            cursor.close()

        cursor = self.cnx.cursor()
        cursor.execute("SELECT * FROM performance_schema.workload_instrumentation WHERE WORKLOAD='api_endpoint_1'")

        rows = 0

        for workload, n_queries, rows_examined, rows_sent, rows_affected, durantion_us in cursor:
            rows += 1
            # All queries were run for WORKLOAD api_endpoint_1 and that's what we are checking
            self.assertEqual(workload, "api_endpoint_1")
            # Check number of queries
            self.assertEqual(n_queries, len(queries))
            # 1 point select + a range scan in the PK for 3 rows + reading the full table (14 rows) for a total of 18
            # rows.
            self.assertEqual(rows_examined, 18)
            # 1 row returned in first query + 3 rows on second + 0 on third = 4 rows returned.
            self.assertEqual(rows_sent, 4)
            # No rows affected in any query since they are all SELECTs
            self.assertEqual(rows_affected, 0)

        # We should have only gotten one row for the workload
        self.assertEqual(rows, 1)
        cursor.close()

        cursor = self.cnx.cursor()
        cursor.execute("SELECT * FROM performance_schema.workload_instrumentation WHERE WORKLOAD='batch_job_1'")

        rows = 0

        for workload, n_queries, rows_examined, rows_sent, rows_affected, durantion_us in cursor:
            rows += 1
            # All queries were run for WORKLOAD batch_job_1 and that's what we are checking
            self.assertEqual(workload, "batch_job_1")
            # Check number of queries - 14 inserts and 1 update
            self.assertEqual(n_queries, 15)
            # 4 rows examined in the UPDATE range scan
            self.assertEqual(rows_examined, 4)
            # 0 rows returned on the writes
            self.assertEqual(rows_sent, 0)
            # 14 rows affected in inserts + 4 in the update = 18 rows
            self.assertEqual(rows_affected, 18)

        # We should have only gotten one row for the workload
        self.assertEqual(rows, 1)
        cursor.close()

    def test_fill_table_capacity(self):
        for I in range(1, 5010):
            query = f"SELECT /* WORKLOAD_NAME=api_endpoint_{I} */ * FROM test_table WHERE id=4"
            cursor = self.cnx.cursor()
            cursor.execute(query)
            for _ in cursor:
                pass
            cursor.close()

        cursor = self.cnx.cursor()
        cursor.execute("SELECT count(*) FROM performance_schema.workload_instrumentation")
        for row in cursor:
            for cnt in row:
                # 5000 real workloads + __UNSPECIFIED__ and __OVERFLOW__
                self.assertEqual(5002, cnt)

        cursor.execute("SELECT COUNT_QUERIES FROM performance_schema.workload_instrumentation WHERE WORKLOAD='__OVERFLOW__'")
        for row in cursor:
            for cnt in row:
                # We ran some queries with workload batch_job_1 in create_test_data and 5009 queries, each one with a
                # different workload api_endpoint_<i> so we must have 11 queries counted as overflow
                self.assertEqual(10, cnt)

    def test_unspecified(self):
        cursor = self.cnx.cursor()
        for I in range(0, 10):
            query = f"SELECT * FROM test_table WHERE id=4"
            cursor.execute(query)
            for _ in cursor:
                pass

        cursor.execute("SELECT /* WORKLOAD_NAME=foo */ COUNT_QUERIES FROM performance_schema.workload_instrumentation WHERE WORKLOAD='__UNSPECIFIED__';")
        for row in cursor:
            for cnt in row:
                # We ran 10 queries without specifying the workload so this should be 10 BUT it's actually 11 because
                # component installation is also counting for one of them
                self.assertEqual(11, cnt)

if __name__ == '__main__':
    unittest.main()
