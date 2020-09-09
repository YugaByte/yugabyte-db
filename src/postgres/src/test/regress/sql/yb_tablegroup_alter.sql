--
-- Tests for altering tablegroups. Includes name / owner changes.
-- Will eventually include tests on pulling tables out of tablegroups / altering the tablegroup of a table etc.
--
CREATE DATABASE db_alter_tablegroup;
\c db_alter_tablegroup

-- Test rename
CREATE TABLEGROUP alter_tgroup1;
CREATE TABLEGROUP alter_tgroup2;

SELECT grpname FROM pg_tablegroup ORDER BY grpname;
ALTER TABLEGROUP alter_tgroup1 RENAME TO alter_tgroup_try_alt;
SELECT grpname FROM pg_tablegroup ORDER BY grpname;
ALTER TABLEGROUP alter_tgroup_try_alt RENAME TO alter_tgroup_alt;
SELECT grpname FROM pg_tablegroup ORDER BY grpname;

ALTER TABLEGROUP alter_tgroup2 RENAME TO alter_tgroup_alt; -- fail
ALTER TABLEGROUP alter_tgroup2 RENAME TO alter_tgroup2; -- fail
SELECT grpname FROM pg_tablegroup ORDER BY grpname;
ALTER TABLEGROUP alter_tgroup_not_exists RENAME TO alter_tgroup_not_exists; -- fail

-- Test alter owner
CREATE USER u1;
CREATE USER u2;
CREATE TABLEGROUP alter_tgroup3 OWNER u1;
CREATE TABLEGROUP alter_tgroup4 OWNER u1;
CREATE TABLEGROUP alter_tgroup5;

ALTER TABLEGROUP alter_tgroup3 OWNER TO u2;
ALTER TABLEGROUP alter_tgroup4 OWNER TO u3; -- fail
ALTER TABLEGROUP alter_tgroup_not_exists OWNER TO u1; -- fail

\c db_alter_tablegroup u1
DROP TABLEGROUP alter_tgroup3; -- fail
DROP TABLEGROUP alter_tgroup4;
ALTER TABLEGROUP alter_tgroup5 OWNER TO u1; -- fail

\c db_alter_tablegroup u2
DROP TABLEGROUP alter_tgroup3;
