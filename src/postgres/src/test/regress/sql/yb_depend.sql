-- Save state of pinned objects
CREATE TEMP TABLE pg_depend_copy AS SELECT * FROM pg_depend WHERE deptype = 'p';
CREATE TEMP TABLE pg_shdepend_copy AS SELECT * FROM pg_shdepend WHERE deptype = 'p';
SELECT CASE WHEN count(*) <@ int8range(6000, 6500) THEN 'OK' ELSE 'NOT OK' END FROM pg_depend_copy; -- number of pinned objects between 6000 and 6500, check YBPinnedObjectsCache sizes if fail
SELECT CASE WHEN count(*) <@ int8range(5, 20) THEN 'OK' ELSE 'NOT OK' END FROM pg_shdepend_copy; -- number of pinned shared objects between 5 and 20, check YBPinnedObjectsCache sizes if fail
CREATE TABLE test_table(k INT PRIMARY KEY, v INT);
CREATE VIEW test_view AS SELECT k FROM test_table WHERE v = 10;
CREATE TYPE type_pair AS (f1 INT, f2 INT);
CREATE TYPE type_pair_with_int AS (f1 type_pair, f2 int);
-- Check list of pinned objects has not been changed
SELECT count(*) FROM pg_depend AS d FULL OUTER JOIN pg_depend_copy AS dC ON (d.refclassid = dC.refclassid AND d.refobjid = dC.refobjid) WHERE (dC.refclassid IS NULL AND d.deptype = 'p') OR d.refclassid IS NULL;
SELECT count(*) FROM pg_shdepend AS d FULL OUTER JOIN pg_shdepend_copy AS dC ON (d.refclassid = dC.refclassid AND d.refobjid = dC.refobjid) WHERE (dC.refclassid IS NULL AND d.deptype = 'p') OR d.refclassid IS NULL;
DROP TABLE test_table; -- should fail
DROP TABLE test_table CASCADE;
DROP TYPE type_pair; -- should fail
DROP TYPE type_pair CASCADE;
