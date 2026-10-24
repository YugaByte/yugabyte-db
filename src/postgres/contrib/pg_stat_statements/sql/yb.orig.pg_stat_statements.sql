CREATE EXTENSION pg_stat_statements;

--
-- simple and compound statements
--
SET pg_stat_statements.track_utility = TRUE;
SET pg_stat_statements.track_planning = TRUE;
-- YB: the default of pg_stat_statements.track is 'all' in YugabyteDB
-- (upstream default: 'top').  Assert it, then pin 'top' so the rest of
-- this test does not depend on the default.
SELECT current_setting('pg_stat_statements.track') AS track;
SET pg_stat_statements.track = 'top';

--
-- create / alter user
--
SELECT pg_stat_statements_reset();
CREATE USER foo PASSWORD 'fooooooo';
ALTER USER foo PASSWORD 'foo2';
CREATE ROLE fizzbuzz PASSWORD 'barrr';
ALTER ROLE fizzbuzz PASSWORD 'barrr2';
-- Passwords of all lengths must be redacted.
CREATE ROLE minimalpass PASSWORD '1';
CREATE ROLE smallpass PASSWORD '123';
CREATE ROLE largepass PASSWORD '123456790123456';
-- A NULL password must also be redacted.
ALTER ROLE fizzbuzz PASSWORD NULL;
-- Password must be redacted irrespective of its position in the query.
ALTER ROLE fizzbuzz SUPERUSER PASSWORD 'test' NOLOGIN;

-- Test that pgss handles pl/pgSQL statements well.
CREATE FUNCTION returnArg(variadic NUMERIC[]) RETURNS NUMERIC LANGUAGE plpgsql AS $$
DECLARE
	r INTEGER := $1[1];
BEGIN
	RETURN r;
END;
$$;
SELECT returnArg(10, 2);
SELECT returnArg(1, 4, 7);

SELECT query, calls, rows FROM pg_stat_statements ORDER BY query COLLATE "C";

--
-- Unprivileged user access: execution metrics visible, query/queryid hidden
-- for other users' entries. Matches vanilla PostgreSQL behavior.
--
SELECT pg_stat_statements_reset();
CREATE ROLE pgss_unpriv LOGIN;

-- Superuser runs queries to populate PGSS
SELECT 1 AS superuser_query;
SELECT 2 + 2 AS superuser_math;

-- Unprivileged user runs a query
SET ROLE pgss_unpriv;
SELECT 'hello from unpriv' AS unpriv_query;
RESET ROLE;

-- Query PGSS as unprivileged user
SET ROLE pgss_unpriv;

-- Own query text and queryid are visible, whereas other users' entries collapse
-- to '<insufficient privilege>' with queryid hidden. Execution metrics (calls,
-- rows, timing) and the latency histogram remain visible for every entry.
SELECT query,
       queryid IS NOT NULL AS has_queryid,
       calls > 0 AS has_calls,
       rows >= 0 AS has_rows,
       total_exec_time >= 0 AS has_exec_time,
       min_exec_time >= 0 AS has_min_time,
       max_exec_time >= 0 AS has_max_time,
       mean_exec_time >= 0 AS has_mean_time,
       yb_latency_histogram IS NOT NULL AS has_histogram
  FROM pg_stat_statements
  WHERE query IN ('SELECT $1 AS unpriv_query', '<insufficient privilege>')
  ORDER BY query COLLATE "C", calls;

RESET ROLE;

-- Granting pg_read_all_stats reveals query text for all entries
GRANT pg_read_all_stats TO pgss_unpriv;
SET ROLE pgss_unpriv;

SELECT query, queryid IS NOT NULL AS has_queryid, calls, rows
  FROM pg_stat_statements
  ORDER BY query COLLATE "C";

RESET ROLE;
REVOKE pg_read_all_stats FROM pgss_unpriv;
DROP ROLE pgss_unpriv;
DROP USER foo;
DROP ROLE fizzbuzz;
DROP ROLE minimalpass;
DROP ROLE smallpass;
DROP ROLE largepass;
DROP EXTENSION pg_stat_statements;
