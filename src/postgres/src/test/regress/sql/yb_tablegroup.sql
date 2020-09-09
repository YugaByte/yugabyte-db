--
-- YB_TABLEGROUP Testsuite: Testing Statments for TABLEGROUP.
--

--
-- pg_catalog alterations. Validate columns of pg_tablegroup and oids.
--
\d pg_tablegroup
SELECT oid, relname, reltype, relnatts FROM pg_class WHERE relname IN ('pg_tablegroup', 'pg_tablegroup_oid_index');
SELECT oid, typname, typrelid FROM pg_type WHERE typname LIKE 'pg_tablegroup';
--
-- CREATE TABLEGROUP
--

CREATE TABLEGROUP tgroup1;
CREATE TABLEGROUP tgroup2;
CREATE TABLEGROUP tgroup3;
CREATE TABLE tgroup_test1 (col1 int, col2 int) TABLEGROUP tgroup1;
CREATE TABLE tgroup_test2 (col1 int, col2 int) TABLEGROUP tgroup1;
CREATE TABLE nogroup (col1 int) NO TABLEGROUP; -- fail
SELECT grpname FROM pg_tablegroup;
SELECT relname
    FROM (SELECT relname, unnest(reloptions) AS opts FROM pg_class) s
    WHERE opts LIKE '%tablegroup%';
CREATE INDEX ON tgroup_test1(col2);
CREATE TABLE tgroup_test3 (col1 int, col2 int) TABLEGROUP tgroup2;
-- Index opt out - should not show up in following SELECT
CREATE INDEX ON tgroup_test3(col1) NO TABLEGROUP;
-- Index explicitly specify tablegroup other than that of indexed table
CREATE INDEX ON tgroup_test3(col1) TABLEGROUP tgroup1;
SELECT s.relname, pg_tablegroup.grpname
    FROM (SELECT relname, unnest(reloptions) AS opts FROM pg_class) s, pg_tablegroup
    WHERE opts LIKE CONCAT('%tablegroup=', CAST(pg_tablegroup.oid AS text), '%');
-- These should fail.
CREATE TABLEGROUP tgroup1;
CREATE TABLE tgroup_test (col1 int, col2 int) TABLEGROUP bad_tgroupname;
CREATE TABLE tgroup_optout (col1 int, col2 int) WITH (colocated=false) TABLEGROUP tgroup1;
CREATE TABLE tgroup_optout (col1 int, col2 int) WITH (colocated=true) TABLEGROUP tgroup1;
CREATE TABLE tgroup_optout (col1 int, col2 int) WITH (colocated=false) TABLEGROUP bad_tgroupname;
CREATE TEMP TABLE tgroup_temp (col1 int, col2 int) TABLEGROUP tgroup1;

--
-- Usage of WITH clause or specifying tablegroup name for CREATE INDEX. These all fail.
--

CREATE TABLE tgroup_with (col1 int, col2 int) WITH (tablegroup=123);
CREATE TABLE tgroup_with (col1 int, col2 int) WITH (tablegroup=123, colocated=true);
CREATE TABLE tgroup_with (col1 int, col2 int) WITH (tablegroup=123) TABLEGROUP tgroup1;
CREATE INDEX ON tgroup_test1(col1) WITH (tablegroup=123);
CREATE INDEX ON tgroup_test1(col1) WITH (tablegroup=123, colocated=true);
CREATE INDEX ON tgroup_test1(col1) WITH (tablegroup=123) TABLEGROUP tgroup1;

--
-- Usage of SPLIT clause with TABLEGROUP should fail
--
CREATE TABLE tgroup_split (col1 int PRIMARY KEY) SPLIT INTO 3 TABLETS TABLEGROUP tgroup1;
CREATE TABLE tgroup_split (col1 int, col2 text) SPLIT INTO 3 TABLETS TABLEGROUP tgroup1;
CREATE INDEX ON tgroup_test1(col1) SPLIT AT VALUES((10), (20), (30));
CREATE INDEX ON tgroup_test1(col1) SPLIT AT VALUES((10), (20), (30)) TABLEGROUP tgroup2;
CREATE INDEX ON tgroup_test1(col1) SPLIT AT VALUES((10), (20), (30)) NO TABLEGROUP; -- should succeed
--
-- Test describes
--
CREATE TABLE tgroup_test4 (col1 int, col2 int) TABLEGROUP tgroup2;
CREATE INDEX ON tgroup_test4(col1);
CREATE INDEX ON tgroup_test4(col2);
-- Add comments
COMMENT ON TABLEGROUP tgroup1 IS 'Comment for Tablegroup 1';
COMMENT ON TABLEGROUP tgroup2 IS 'Comment for Tablegroup 2';
\dgr
\dgr+
\dgrt
\dgrt+
COMMENT ON TABLEGROUP tgroup2 IS NULL;
\dgr+ tgroup2
\dgrt tgroup2

-- Describe table
\d tgroup_test2
\d tgroup_test4
\d tgroup_test4_col1_idx
CREATE TABLEGROUP tgroup_describe1;
CREATE TABLEGROUP tgroup_describe2;
CREATE TABLE tgroup_describe (col1 int) TABLEGROUP tgroup_describe1;
CREATE INDEX ON tgroup_describe(col1);
CREATE INDEX ON tgroup_describe(col1) NO TABLEGROUP;
CREATE INDEX ON tgroup_describe(col1) TABLEGROUP tgroup_describe2;
\d tgroup_describe
--
-- DROP TABLEGROUP
--

DROP TABLEGROUP tgroup3;
-- These should fail. CREATE TABLE is to check that the row entry was deleted from pg_tablegroup.
CREATE TABLE tgroup_test5 (col1 int, col2 int) TABLEGROUP tgroup3;
DROP TABLEGROUP tgroup1;
DROP TABLEGROUP bad_tgroupname;
-- This drop should work now.
DROP TABLE tgroup_test1;
DROP TABLE tgroup_test2;
DROP INDEX tgroup_test3_col1_idx1;
DROP TABLEGROUP tgroup1;
-- Create a tablegroup with the name of a dropped tablegroup.
CREATE TABLEGROUP tgroup1;

--
-- Interactions with colocated database.
--

CREATE DATABASE db_colocated colocated=true;
\c db_colocated
-- These should fail.
CREATE TABLEGROUP tgroup1;
CREATE TABLE tgroup_test (col1 int, col2 int) TABLEGROUP tgroup1;
CREATE TABLE tgroup_optout (col1 int, col2 int) WITH (colocated=false) TABLEGROUP tgroup1;
