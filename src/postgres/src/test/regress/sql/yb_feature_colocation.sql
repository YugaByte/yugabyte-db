--
-- Colocation
--

-- CREATE TABLE on non-colocated database

CREATE TABLE tab_colo (a INT) WITH (colocated = true);
CREATE TABLE tab_noco (a INT) WITH (colocated = false);
DROP TABLE tab_noco;

-- CREATE DATABASE colocated

CREATE DATABASE colocation_test colocated = true;
\c colocation_test

-- CREATE TABLE

CREATE TABLE tab_nonkey (a INT);
\d tab_nonkey
CREATE TABLE tab_key (a INT PRIMARY KEY);
\d tab_key
CREATE TABLE tab_range (a INT, PRIMARY KEY (a ASC));
CREATE TABLE tab_range_nonkey (a INT, b INT, PRIMARY KEY (a ASC));
-- opt out of using colocated tablet
CREATE TABLE tab_nonkey_noco (a INT) WITH (colocated = false);
-- colocated tables with no primary keys should not be hash partitioned
CREATE TABLE split_table ( a integer, b text ) SPLIT INTO 4 TABLETS;
-- multi column primary key table
CREATE TABLE tab_range_range (a INT, b INT, PRIMARY KEY (a, b DESC));
CREATE TABLE tab_range_colo (a INT, PRIMARY KEY (a ASC)) WITH (colocated = true);

INSERT INTO tab_range (a) VALUES (0), (1), (2);
INSERT INTO tab_range (a, b) VALUES (0, '0'); -- fail
INSERT INTO tab_range_nonkey (a, b) VALUES (0, '0'), (1, '1');
INSERT INTO tab_nonkey_noco (a) VALUES (0), (1), (2), (3);
INSERT INTO tab_range_range (a, b) VALUES (0, 0), (0, 1), (1, 0), (1, 1);
INSERT INTO tab_range_colo (a) VALUES (0), (1), (2), (3);

SELECT * FROM tab_range;
SELECT * FROM tab_range WHERE a = 2;
SELECT * FROM tab_range WHERE n = '0'; -- fail
SELECT * FROM tab_range_nonkey;
SELECT * FROM tab_nonkey_noco ORDER BY a ASC;
SELECT * FROM tab_range_range;
SELECT * FROM tab_range_colo;

BEGIN;
INSERT INTO tab_range_colo VALUES (4);
SELECT * FROM tab_range_colo;
ROLLBACK;
BEGIN;
INSERT INTO tab_range_colo VALUES (5);
COMMIT;
SELECT * FROM tab_range_colo;

INSERT INTO tab_range_colo VALUES (6), (6);

-- CREATE INDEX

-- table with index
CREATE TABLE tab_range_nonkey2 (a INT, b INT, PRIMARY KEY (a ASC));
CREATE INDEX idx_range ON tab_range_nonkey2 (a);
\d tab_range_nonkey2
INSERT INTO tab_range_nonkey2 (a, b) VALUES (0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5);
EXPLAIN (COSTS OFF) SELECT * FROM tab_range_nonkey2 WHERE a = 1;
SELECT * FROM tab_range_nonkey2 WHERE a = 1;
UPDATE tab_range_nonkey2 SET b = b + 1 WHERE a > 3;
SELECT * FROM tab_range_nonkey2;
DELETE FROM tab_range_nonkey2 WHERE a > 3;
SELECT * FROM tab_range_nonkey2;

-- colocated table with non-colocated index
CREATE TABLE tab_range_nonkey3 (a INT, b INT, PRIMARY KEY (a ASC));
CREATE INDEX idx_range_colo ON tab_range_nonkey3 (a) WITH (colocated = true);

-- colocated table with colocated index
CREATE TABLE tab_range_nonkey4 (a INT, b INT, PRIMARY KEY (a ASC));
CREATE INDEX idx_range_noco ON tab_range_nonkey4 (a) WITH (colocated = false);

-- non-colocated table with index
CREATE TABLE tab_range_nonkey_noco (a INT, b INT, PRIMARY KEY (a ASC)) WITH (colocated = false);
CREATE INDEX idx_range2 ON tab_range_nonkey_noco (a);
INSERT INTO tab_range_nonkey_noco (a, b) VALUES (0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5);
EXPLAIN (COSTS OFF) SELECT * FROM tab_range_nonkey_noco WHERE a = 1;
SELECT * FROM tab_range_nonkey_noco WHERE a = 1;
UPDATE tab_range_nonkey_noco SET b = b + 1 WHERE a > 3;
SELECT * FROM tab_range_nonkey_noco;
DELETE FROM tab_range_nonkey_noco WHERE a > 3;
SELECT * FROM tab_range_nonkey_noco;

-- more tables and indexes
CREATE TABLE tab_range_nonkey_noco2 (a INT, b INT, PRIMARY KEY (a ASC)) WITH (colocated = false);
CREATE INDEX idx_range3 ON tab_range_nonkey_noco2 (a);
INSERT INTO tab_range_nonkey_noco2 (a, b) VALUES (0, 0);
CREATE TABLE tab_range_nonkey_noco3 (a INT, b INT, PRIMARY KEY (a ASC)) WITH (colocated = false);
CREATE INDEX idx_range4 ON tab_range_nonkey_noco3 (a);
CREATE TABLE tab_range_nonkey5 (a INT, b INT, PRIMARY KEY (a ASC));
CREATE INDEX idx_range5 ON tab_range_nonkey5 (a);

\dt
\di

-- TRUNCATE TABLE

-- truncate colocated table with default index
TRUNCATE TABLE tab_range;
SELECT * FROM tab_range;
INSERT INTO tab_range VALUES (4);
SELECT * FROM tab_range;
INSERT INTO tab_range VALUES (1);
INSERT INTO tab_range VALUES (2), (5);
SELECT * FROM tab_range;
DELETE FROM tab_range WHERE a = 2;
TRUNCATE TABLE tab_range;
SELECT * FROM tab_range;
INSERT INTO tab_range VALUES (2);
SELECT * FROM tab_range;

TRUNCATE TABLE tab_range;

-- truncate non-colocated table without index
TRUNCATE TABLE tab_nonkey_noco;
SELECT * FROM tab_nonkey_noco;

-- truncate colocated table with explicit index
TRUNCATE TABLE tab_range_nonkey2;
SELECT * FROM tab_range_nonkey2;

-- truncate non-colocated table with explicit index
TRUNCATE TABLE tab_range_nonkey_noco2;
SELECT * FROM tab_range_nonkey_noco2;

\dt
\di

-- ALTER TABLE
INSERT INTO tab_range (a) VALUES (0), (1), (2);
INSERT INTO tab_range_nonkey2 (a, b) VALUES (0, 0), (1, 1);

SELECT * FROM tab_range;
SELECT * FROM tab_range_nonkey2;

-- Alter colocated tables
ALTER TABLE tab_range ADD COLUMN x INT;
ALTER TABLE tab_range_nonkey2 DROP COLUMN b;

SELECT * FROM tab_range;
SELECT * FROM tab_range_nonkey2;

ALTER TABLE tab_range_nonkey2 RENAME TO tab_range_nonkey2_renamed;
SELECT * FROM tab_range_nonkey2_renamed;
SELECT * FROM tab_range_nonkey2;

-- DROP TABLE

-- drop colocated table with default index
DROP TABLE tab_range;
SELECT * FROM tab_range;

-- drop non-colocated table without index
DROP TABLE tab_nonkey_noco;
SELECT * FROM tab_nonkey_noco;

--- drop colocated table with explicit index
DROP TABLE tab_range_nonkey2_renamed;
SELECT * FROM tab_range_nonkey2_renamed;

-- drop non-colocated table with explicit index
DROP TABLE tab_range_nonkey_noco2;
SELECT * FROM tab_range_nonkey_noco2;

-- DROP INDEX

-- drop index on non-colocated table
DROP INDEX idx_range2;
EXPLAIN SELECT * FROM tab_range_nonkey_noco WHERE a = 1;

-- drop index on colocated table
DROP INDEX idx_range5;
EXPLAIN SELECT * FROM tab_range_nonkey5 WHERE a = 1;

\dt
\di

-- drop database
\c yugabyte
DROP DATABASE colocation_test;
\c colocation_test
