--
-- FOREIGN KEY (YB-added tests)
--
-- TODO: Run this test with REPEATABLE READ isolation level:
--       https://github.com/yugabyte/yugabyte-db/issues/2604
--
-- MATCH FULL
--
-- First test, check and cascade
--


CREATE TABLE ITABLE ( ptest1 int, ptest2 text );
CREATE UNIQUE INDEX ITABLE_IDX ON ITABLE(ptest1);
CREATE TABLE FKTABLE ( ftest1 int REFERENCES ITABLE(ptest1) MATCH FULL ON DELETE CASCADE ON UPDATE CASCADE, ftest2 int );

-- Insert test data into ITABLE
INSERT INTO ITABLE VALUES (1, 'Test1');
INSERT INTO ITABLE VALUES (2, 'Test2');
INSERT INTO ITABLE VALUES (3, 'Test3');
INSERT INTO ITABLE VALUES (4, 'Test4');
INSERT INTO ITABLE VALUES (5, 'Test5');

-- Insert successful rows into FK TABLE
INSERT INTO FKTABLE VALUES (1, 2);
INSERT INTO FKTABLE VALUES (2, 3);
INSERT INTO FKTABLE VALUES (3, 4);
INSERT INTO FKTABLE VALUES (NULL, 1);

-- Insert a failed row into FK TABLE
INSERT INTO FKTABLE VALUES (100, 2);

-- Check FKTABLE
SELECT * FROM FKTABLE ORDER BY ftest1;

-- Delete a row from ITABLE
DELETE FROM ITABLE WHERE ptest1=1;

-- Check FKTABLE for removal of matched row
SELECT * FROM FKTABLE ORDER BY ftest1;

-- Update a row from ITABLE
UPDATE ITABLE SET ptest1=1 WHERE ptest1=2;

-- Check FKTABLE for update of matched row
SELECT * FROM FKTABLE ORDER BY ftest1;

DROP TABLE FKTABLE;
DROP TABLE ITABLE;

--
-- check set NULL and table constraint on multiple columns
--
CREATE TABLE ITABLE ( ptest1 int, ptest2 int, ptest3 text);
CREATE UNIQUE INDEX ITABLE_IDX ON ITABLE(ptest1, ptest2);

CREATE TABLE FKTABLE ( ftest1 int, ftest2 int, ftest3 int, CONSTRAINT constrname FOREIGN KEY(ftest1, ftest2)
                       REFERENCES ITABLE(ptest1, ptest2) MATCH FULL ON DELETE SET NULL ON UPDATE SET NULL);


-- Insert test data into ITABLE
INSERT INTO ITABLE VALUES (1, 2, 'Test1');
INSERT INTO ITABLE VALUES (1, 3, 'Test1-2');
INSERT INTO ITABLE VALUES (2, 4, 'Test2');
INSERT INTO ITABLE VALUES (3, 6, 'Test3');
INSERT INTO ITABLE VALUES (4, 8, 'Test4');
INSERT INTO ITABLE VALUES (5, 10, 'Test5');

-- Insert successful rows into FK TABLE
INSERT INTO FKTABLE VALUES (1, 2, 4);
INSERT INTO FKTABLE VALUES (1, 3, 5);
INSERT INTO FKTABLE VALUES (2, 4, 8);
INSERT INTO FKTABLE VALUES (3, 6, 12);
INSERT INTO FKTABLE VALUES (NULL, NULL, 0);

-- Insert failed rows into FK TABLE
INSERT INTO FKTABLE VALUES (100, 2, 4);
INSERT INTO FKTABLE VALUES (2, 2, 4);
INSERT INTO FKTABLE VALUES (NULL, 2, 4);
INSERT INTO FKTABLE VALUES (1, NULL, 4);

-- Check FKTABLE
SELECT * FROM FKTABLE ORDER BY ftest1, ftest2, ftest3;

-- Delete a row from ITABLE
DELETE FROM ITABLE WHERE ptest1=1 and ptest2=2;

-- Check that values of matched row are set to null.
SELECT * FROM FKTABLE ORDER BY ftest1, ftest2, ftest3;

-- Delete another row from ITABLE
DELETE FROM ITABLE WHERE ptest1=5 and ptest2=10;

-- Check FKTABLE (should be no change)
SELECT * FROM FKTABLE ORDER BY ftest1, ftest2, ftest3;

-- Update a row from ITABLE
UPDATE ITABLE SET ptest1=1 WHERE ptest1=2;

-- Check FKTABLE for update of matched row
SELECT * FROM FKTABLE ORDER BY ftest1, ftest2, ftest3;

-- Test truncate

-- Should fail due to dependency on FKTABLE.
TRUNCATE ITABLE;

-- Should truncate both ITABLE and FKTABLE.
TRUNCATE ITABLE CASCADE;

-- Check that both tables are now empty.
SELECT * FROM ITABLE ORDER BY ptest1, ptest2;
SELECT * FROM FKTABLE ORDER BY ftest1, ftest2, ftest3;

DROP TABLE ITABLE CASCADE;
DROP TABLE FKTABLE;

-- UPDATE with multiple foreign keys --
CREATE TABLE pk(k INT PRIMARY KEY, x INT UNIQUE, y INT UNIQUE);
CREATE TABLE fk_primary(k INT REFERENCES pk(k));
CREATE TABLE fk_unique_x(x INT REFERENCES pk(x));
CREATE TABLE fk_unique_y(y INT REFERENCES pk(y));
INSERT INTO pk VALUES(1, 10, 100);

UPDATE pk SET x = 11 WHERE x = 10;
UPDATE pk SET y = 101 WHERE y = 100;

INSERT INTO fk_unique_x VALUES(11);
INSERT INTO fk_unique_y VALUES(101);

UPDATE pk SET x = 12 WHERE x = 11;
UPDATE pk SET y = 102 WHERE y = 101;

DROP TABLE fk_unique_y;
DROP TABLE fk_unique_x;
DROP TABLE fk_primary;
DROP TABLE pk;

-- DEFERRABLE foreign key constraints
CREATE TABLE parent(k INT PRIMARY KEY);
CREATE TABLE child(k INT PRIMARY KEY, p_fk INT REFERENCES parent INITIALLY DEFERRED);

BEGIN;
INSERT INTO child VALUES (1, 1);
INSERT INTO child VALUES (2, 2);
COMMIT; -- should fail

SELECT * FROM child ORDER BY k;

BEGIN;
INSERT INTO child VALUES (1, 1);
INSERT INTO child VALUES (2, 1);
INSERT INTO child VALUES (3, 2), (4, 3), (5, 4);
INSERT INTO parent VALUES (1);
INSERT INTO parent VALUES (2), (3), (4);
COMMIT;

SELECT * FROM parent ORDER BY k;
SELECT * FROM child ORDER BY k;

DELETE FROM child;
DELETE FROM parent;

CREATE TABLE grand_child(k INT PRIMARY KEY, c_fk INT REFERENCES child INITIALLY DEFERRED);

BEGIN;
INSERT INTO grand_child VALUES (1, 1), (2, 2), (3, 3);
INSERT INTO child VALUES (2, 2), (3, 3);
INSERT INTO parent VALUES (1), (2);
COMMIT; -- should fail

BEGIN;
INSERT INTO grand_child VALUES (1, 1), (2, 2), (3, 3);
INSERT INTO child VALUES (2, 2), (3, 3), (1, 1);
INSERT INTO parent VALUES (3), (2), (1);
COMMIT;

SELECT * FROM parent ORDER BY k;
SELECT * FROM child ORDER BY k;
SELECT * FROM grand_child ORDER BY k;

BEGIN;
UPDATE grand_child SET c_fk = 4 WHERE k = 1;
INSERT INTO child VALUES (4, 4);
INSERT INTO parent VALUES (5);
COMMIT; -- should fail

BEGIN;
UPDATE grand_child SET c_fk = 4 WHERE k = 1;
INSERT INTO child VALUES (4, 4);
INSERT INTO parent VALUES (4);
COMMIT;

SELECT * FROM parent ORDER BY k;
SELECT * FROM child ORDER BY k;
SELECT * FROM grand_child ORDER BY k;

DROP TABLE grand_child;
DROP TABLE child;
DELETE FROM parent;

CREATE TABLE child(k INT PRIMARY KEY, p_fk INT REFERENCES parent INITIALLY DEFERRED);
CREATE TABLE grand_child(k INT PRIMARY KEY, c_fk INT REFERENCES child);

BEGIN;
INSERT INTO child VALUES (1, 1), (2, 2);
INSERT INTO grand_child VALUES (1, 1), (2, 2);
INSERT INTO parent VALUES (1);
COMMIT; -- should fail

BEGIN;
INSERT INTO child VALUES (1, 1), (2, 2);
INSERT INTO grand_child VALUES (1, 1), (2, 2);
INSERT INTO parent VALUES (1);
INSERT INTO parent VALUES (2);
COMMIT;

SELECT * FROM parent ORDER BY k;
SELECT * FROM child ORDER BY k;
SELECT * FROM grand_child ORDER BY k;

DROP TABLE grand_child;
DROP TABLE child;
DROP TABLE parent;

CREATE TABLE mother(k INT PRIMARY KEY);
CREATE TABLE father(k INT PRIMARY KEY);
CREATE TABLE child(
    k INT PRIMARY KEY,
    m_fk INT REFERENCES mother INITIALLY DEFERRED,
    f_fk INT REFERENCES father INITIALLY DEFERRED);

BEGIN;
INSERT INTO child VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);
INSERT INTO mother VALUES (1), (2);
INSERT INTO father VALUES (1), (2), (3);
COMMIT; -- should fail

BEGIN;
INSERT INTO child VALUES (1, 1, 1), (2, 2, 2);
INSERT INTO mother VALUES (1), (2);
INSERT INTO father VALUES (3), (2), (1);
COMMIT;

SELECT * FROM child ORDER BY k;
SELECT * FROM mother ORDER by k;
SELECT * FROM father ORDER BY k;

BEGIN;
UPDATE child SET m_fk = 4, f_fk = 4 WHERE k = 1;
INSERT INTO mother VALUES (5), (4);
INSERT INTO father VALUES (4), (5);
COMMIT;

SELECT * FROM child ORDER BY k;
SELECT * FROM mother ORDER by k;
SELECT * FROM father ORDER BY k;

DROP TABLE child;
DELETE FROM mother;
DELETE FROM father;

CREATE TABLE child(
    k INT PRIMARY KEY,
    m_fk INT REFERENCES mother,
    f_fk INT REFERENCES father INITIALLY DEFERRED);

BEGIN;
INSERT INTO mother VALUES (1), (2);
INSERT INTO child VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3); -- should fail
COMMIT;

BEGIN;
INSERT INTO mother VALUES (1), (2);
INSERT INTO child VALUES (1, 1, 1), (2, 2, 2);
INSERT INTO father VALUES (1);
COMMIT; -- should fail

BEGIN;
INSERT INTO mother VALUES (1), (2);
INSERT INTO child VALUES (1, 1, 1), (2, 2, 2);
INSERT INTO father VALUES (3), (2), (1);
COMMIT;

SELECT * FROM child ORDER BY k;
SELECT * FROM mother ORDER by k;
SELECT * FROM father ORDER BY k;

DROP TABLE child;
DELETE FROM mother;
DELETE FROM father;

CREATE TABLE child(
    k INT PRIMARY KEY,
    m_fk INT REFERENCES mother INITIALLY DEFERRED,
    f_fk INT REFERENCES father);

BEGIN;
INSERT INTO father VALUES (1), (2);
INSERT INTO child VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3); -- should fail
COMMIT;

BEGIN;
INSERT INTO father VALUES (1), (2);
INSERT INTO child VALUES (1, 1, 1), (2, 2, 2);
INSERT INTO mother VALUES (1);
COMMIT; -- should fail

BEGIN;
INSERT INTO father VALUES (1), (2);
INSERT INTO child VALUES (1, 1, 1), (2, 2, 2);
INSERT INTO mother VALUES (3), (2), (1);
COMMIT;

SELECT * FROM child ORDER BY k;
SELECT * FROM mother ORDER by k;
SELECT * FROM father ORDER BY k;

BEGIN;
UPDATE child SET f_fk = 4 WHERE k = 1; -- should fail
COMMIT;

BEGIN;
UPDATE child SET m_fk = 4 WHERE k = 1;
INSERT INTO mother VALUES (4);
COMMIT;

SELECT * FROM child ORDER BY k;
SELECT * FROM mother ORDER by k;

DROP TABLE child;
DROP TABLE mother;
DROP TABLE father;

CREATE TABLE p1(k INT PRIMARY KEY);
CREATE TABLE p2(k INT PRIMARY KEY);
CREATE TABLE p3(k INT PRIMARY KEY);
CREATE TABLE p4(k INT PRIMARY KEY);
CREATE TABLE c(
    k INT PRIMARY KEY,
    p1_fk INT REFERENCES p1 INITIALLY DEFERRED,
    p2_fk INT REFERENCES p2,
    p3_fk INT REFERENCES p3 INITIALLY DEFERRED,
    p4_fk INT REFERENCES p4);

BEGIN;
INSERT INTO p2 VALUES (1), (2);
INSERT INTO c VALUES (1, 1, 1, 1, 1); -- should fail
COMMIT;

BEGIN;
INSERT INTO p2 VALUES (1), (2);
INSERT INTO p4 VALUES (1);
INSERT INTO c VALUES (1, 1, 1, 1, 1);
INSERT INTO p3 VALUES (1);
COMMIT;  -- should fail

BEGIN;
INSERT INTO p2 VALUES (1), (2);
INSERT INTO p4 VALUES (1), (2);
INSERT INTO c VALUES (1, 1, 1, 1, 1), (2, 2, 2, 2, 2);
INSERT INTO p3 VALUES (2), (1);
INSERT INTO p1 VALUES (3), (2), (1);
COMMIT;

SELECT * FROM p1 ORDER BY k;
SELECT * FROM p2 ORDER BY k;
SELECT * FROM p3 ORDER BY k;
SELECT * FROM p4 ORDER BY k;
SELECT * FROM c ORDER BY k;

CREATE TABLE parent(k INT PRIMARY KEY, v INT UNIQUE);
CREATE TABLE child_1(k INT PRIMARY KEY, v INT REFERENCES parent(k));
CREATE TABLE child_2(k INT PRIMARY KEY, v INT REFERENCES parent(k));

DO $$
BEGIN
  INSERT INTO parent values(1, 10);
  INSERT INTO child_1 values(20, 1);
  DELETE FROM child_1;
  DELETE FROM parent;
  INSERT INTO child_2 values(30, 1); -- should fail
END $$;

DROP TABLE child_1;
DROP TABLE child_2;

CREATE TABLE child_1(k INT PRIMARY KEY, v INT REFERENCES parent(v));
CREATE TABLE child_2(k INT PRIMARY KEY, v INT REFERENCES parent(v));

DO $$
BEGIN
  INSERT INTO parent values(1, 10);
  INSERT INTO child_2 values(30, 10);
  DELETE FROM child_2;
  DELETE FROM parent;
  INSERT INTO child_1 values(20, 10); -- should fail
END $$;

DROP TABLE child_1;

CREATE TABLE child_1(k INT PRIMARY KEY, v INT REFERENCES parent(k));

INSERT INTO parent SELECT s, 1000000 + s FROM generate_series(1, 10000) AS s;
INSERT INTO child_1 SELECT s, s FROM generate_series(1, 10001) AS s; -- should fail
INSERT INTO child_2 SELECT s, 1000000 + s FROM generate_series(1, 10001) AS s; -- should fail

INSERT INTO child_1 SELECT s, s FROM generate_series(1, 10000) AS s;
INSERT INTO child_2 SELECT s, 1000000 + s FROM generate_series(1, 10000) AS s;

UPDATE child_1 SET v = v * 2 WHERE k <= 5001; -- should fail
UPDATE child_2 SET v = (v - 1000000) * 2 + 1000000 WHERE k <= 5001; -- should fail

UPDATE child_1 SET v = v * 2 WHERE k < 5001;
UPDATE child_2 SET v = (v - 1000000) * 2 + 1000000 WHERE k < 5001;
