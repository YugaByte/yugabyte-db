--
-- Tests basic CURSOR support.
--
CREATE TABLE test_cursor (id int PRIMARY KEY, name varchar);
--
-- Use CURSOR for SELECT in anonymous SQL block.
--
INSERT INTO test_cursor VALUES (1, 'one'), (2, 'two'), (3, 'three');
BEGIN;
  DECLARE c_sql CURSOR FOR SELECT * FROM test_cursor;
  FETCH ALL FROM c_sql;
END;
-- CURSOR in anonymous PL/PGSQL block.
DO LANGUAGE plpgsql $$
DECLARE
  c_pl CURSOR FOR SELECT * FROM test_cursor;
  row RECORD;
BEGIN
  OPEN c_pl;
  LOOP
    FETCH FROM c_pl INTO row;
    EXIT WHEN NOT FOUND;
    RAISE NOTICE 'FETCHING a row: %', row;
  END LOOP;
END $$;
-- CURSOR in PL/PGSQL procedure.
CREATE OR REPLACE PROCEDURE proc() LANGUAGE plpgsql AS $body$
DECLARE
  c_proc CURSOR FOR SELECT * FROM test_cursor;
  c_ref REFCURSOR;
  row1 RECORD;
  row2 RECORD;
BEGIN
  OPEN c_proc;
  OPEN c_ref FOR SELECT * FROM test_cursor;
  LOOP
    FETCH FROM c_proc INTO row1;
    EXIT WHEN NOT FOUND;
    FETCH FROM c_ref INTO row2;
    EXIT WHEN NOT FOUND;
    RAISE NOTICE 'FETCHING a row: %, %', row1, row2;
  END LOOP;
END
$body$;
--
CALL proc();
--
DELETE FROM test_cursor;
--
-- Issue 6627 CURSOR query read time should be set at DECLARE or OPEN instead of FETCH.
--
-- CURSOR in SQL block.
BEGIN;
  INSERT INTO test_cursor VALUES (1, 'one');
  DECLARE c CURSOR FOR SELECT * FROM test_cursor;
  INSERT INTO test_cursor VALUES (2, 'two'), (3, 'three');
  -- BUG: Only one row should be output by the following FETCH, but there's 3.
  FETCH ALL FROM c;
ROLLBACK;
-- CURSOR in PL/PGSQL block (Issue 6627).
DO LANGUAGE plpgsql $$
DECLARE
  c_pl CURSOR FOR SELECT * FROM test_cursor;
  row RECORD;
BEGIN
  INSERT INTO test_cursor VALUES (1, 'one');
  OPEN c_pl;
  INSERT INTO test_cursor VALUES (2, 'two'), (3, 'three');
  -- BUG: Only one row should be output by the following LOOP, but there's 3.
  LOOP
    FETCH FROM c_pl INTO row;
    EXIT WHEN NOT FOUND;
    RAISE NOTICE 'FETCHING a row: %', row;
  END LOOP;
  DELETE FROM test_cursor;
END $$;
-- CURSOR in PL/PGSQL procedure (Issue 6627).
CREATE OR REPLACE PROCEDURE proc() LANGUAGE plpgsql AS $body$
DECLARE
  c_proc CURSOR FOR SELECT * FROM test_cursor;
  row RECORD;
BEGIN
  INSERT INTO test_cursor VALUES (1, 'one');
  OPEN c_proc;
  INSERT INTO test_cursor VALUES (2, 'two'), (3, 'three');
  -- BUG: Only one row should be output by the following LOOP, but there's 3.
  LOOP
    FETCH FROM c_proc INTO row;
    EXIT WHEN NOT FOUND;
    RAISE NOTICE 'FETCHING a row: %', row;
  END LOOP;
  DELETE FROM test_cursor;
END
$body$;
--
CALL proc();
--
-- Use CURSOR to call FUNCTION in anonymous block. 
--
CREATE FUNCTION cnt() RETURNS BIGINT AS
'select count(*) from test_cursor' LANGUAGE sql;
--
CREATE FUNCTION cnt_v() RETURNS BIGINT AS
'select count(*) from test_cursor' LANGUAGE sql volatile;
--
CREATE FUNCTION cnt_s() RETURNS BIGINT AS
'select count(*) from test_cursor' LANGUAGE sql stable;
--
CREATE FUNCTION cnt_i() RETURNS BIGINT AS
'select count(*) from test_cursor' LANGUAGE sql immutable;
--
BEGIN;
  INSERT INTO test_cursor VALUES (1, 1), (2, 2), (3, 3);
  DECLARE c_cnt CURSOR FOR SELECT cnt(), cnt_v(), cnt_s(), cnt_i();
  FETCH ALL FROM c_cnt;
ROLLBACK;
--
-- Issue 6629 STABLE execution time should be set at DECLARE or OPEN instead of FETCH.
--
BEGIN;
  INSERT INTO test_cursor VALUES (1, 'one');
  DECLARE c_cnt CURSOR FOR SELECT cnt(), cnt_v(), cnt_s(), cnt_i();
  INSERT INTO test_cursor VALUES (2, 'two'), (3, 'three');
  -- BUG: Only one row should be output by the following FETCH.
  FETCH ALL FROM c_cnt;
ROLLBACK;
--
-- CURSOR for SELECT constant
--
DECLARE c_const CURSOR WITH HOLD FOR SELECT 1 as const;
FETCH FROM c_const;
CLOSE c_const;
--
-- DISCARDed CURSOR
--
DECLARE c_discard CURSOR WITH HOLD FOR SELECT 1;
DISCARD ALL;
FETCH FROM c_discard;
