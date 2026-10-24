-- Test for YBSTAT
-- Verifies behavior for yb created system views and tables.

-- Testing to see if yb_terminated_queries is populated correctly
-- For TEMP_FILE_LIMIT under the SET configurations
-- These tests are with superuser, so we can observe the terminated queries
-- across databases, not just the one we're connected to.
SET work_mem TO 64;
SET temp_file_limit TO 0;
SELECT * FROM generate_series(0, 1000000);
SELECT 'bob' FROM generate_series(0, 1000000);

-- This sleep is necessary to allow PGSTAT_STAT_INTERVAL (500) milliseconds to pass.
-- In that time, the statistics in the backend would be updated and we will read updated
-- statistics.
SELECT pg_sleep(1);

\d yb_terminated_queries
SELECT databasename, termination_reason, query_text FROM yb_terminated_queries;
SELECT databasename, termination_reason, query_text FROM yb_terminated_queries WHERE databasename = 'yugabyte';

CREATE DATABASE db2;
\c db2

SELECT databasename, termination_reason, query_text FROM yb_terminated_queries;

SET work_mem TO 64;
SET temp_file_limit TO 0;
SELECT * FROM generate_series(0, 1000001);
SELECT pg_sleep(1);
SELECT databasename, termination_reason, query_text FROM yb_terminated_queries;

-- Test permissions for different roles
\c yugabyte
SELECT databasename, termination_reason, query_text FROM yb_terminated_queries;

CREATE ROLE test_user WITH login;
\c yugabyte test_user
SELECT * FROM generate_series(0, 100000002);
SELECT pg_sleep(1);
SELECT
    D.datname AS databasename,
    S.query_text AS query_text
FROM yb_pg_stat_get_queries(null) AS S
LEFT JOIN pg_database AS D ON (S.db_oid = D.oid) ORDER BY S.db_oid;

\c yugabyte yugabyte
GRANT pg_read_all_stats TO test_user;
\c yugabyte test_user
SELECT
    D.datname AS databasename,
    S.query_text AS query_text
FROM yb_pg_stat_get_queries(null) AS S
LEFT JOIN pg_database AS D ON (S.db_oid = D.oid) ORDER BY S.db_oid;

\c yugabyte yugabyte
REVOKE pg_read_all_stats FROM test_user;
GRANT yb_db_admin TO test_user;
\c yugabyte test_user
SELECT
    D.datname AS databasename,
    S.query_text AS query_text
FROM yb_pg_stat_get_queries(null) AS S
LEFT JOIN pg_database AS D ON (S.db_oid = D.oid) ORDER BY S.db_oid;

\c yugabyte yugabyte
REVOKE yb_db_admin FROM test_user;

ALTER ROLE test_user WITH superuser;
ALTER ROLE test_user WITH createdb;

\c yugabyte test_user
CREATE DATABASE test_user_database;
\c test_user_database test_user

SET work_mem TO 128;

-- Some shenanigans with temp_file_limit means you need to be superuser
-- to change the value of this config.
SET temp_file_limit TO 0;
show temp_file_limit;

SELECT * FROM generate_series(0, 1234567);
SELECT databasename, termination_reason, query_text FROM yb_terminated_queries ORDER BY databasename;

-- Drop the superuser privilege as we want to see if we would only see the terminated query
-- of our created database only.
ALTER user test_user WITH nosuperuser;

SELECT pg_sleep(1);
SELECT databasename, termination_reason, query_text FROM yb_terminated_queries;

SET work_mem TO 64;
SELECT 'We were taught in this modern age that science and mathematics is the pinnacle of human achievement.'
'Yet, in our complacency, we began to neglect the very thing which our ancestors had once done: to challenge the process.'
'We need to stand back and critically analyze what we do and doing so would allow us to become better and so much more.'
FROM generate_series(0, 1000000);

SELECT pg_sleep(1);
SELECT length(query_text) > 255 AS query_text_exceeds_255,
       query_text LIKE '%FROM generate_series(0, 1000000);' AS query_text_is_complete
FROM yb_terminated_queries
WHERE query_text LIKE 'SELECT ''We were taught%';
