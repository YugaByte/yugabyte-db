--------------------------------------
-- Hash-partitioned tables/indexes.
--------------------------------------

CREATE TABLE t1(h int, r int, v1 int, v2 int, v3 int, primary key(h HASH, r ASC));
CREATE INDEX t1_v1_v2_idx on t1(v1 HASH, v2 ASC);
CREATE UNIQUE INDEX t1_v3_uniq_idx on t1(v3 HASH);

--------------------------------------
-- Test unique vs non-unique indexes.

-- Expect to use t1_v3_uniq_idx because will guarantee a single-row scan (due to uniqueness).
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 and v2 = 1 and v3 = 1;

-- Expect to use t1_v1_v2_idx because the inequality condition on v3 (HASH) is not useful.
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 and v2 = 1 and v3 > 1;
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 and v2 > 1 and v3 > 1;

--------------------------------------
-- Test covered vs uncovered indexes.

-- Should prioritize the pkey index because it covers all columns (index only scan).
EXPLAIN SELECT * FROM t1 WHERE h = 1 and v1 = 1;
EXPLAIN SELECT * FROM t1 WHERE h = 1 and r = 2 and v1 = 1 and v2 = 2;

-- Should prioritize the t1_v1_v2_idx because it is fully specified.
EXPLAIN SELECT * FROM t1 WHERE h = 1 and v1 = 1 and v2 = 2;

--------------------------------------
-- Test partial indexes.

-- Should use t1_v1_v2_idx because conditions partly match.
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 AND v3 > 1;
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 AND v3 > 2;
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 AND v3 > 3;

CREATE INDEX t1_v1_v2_uniq_partial_idx ON t1(v1, v2) WHERE v3 > 2;

-- Should still use t1_v1_v2_idx because partial index condition does not match.
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 AND v3 > 1;

-- Should both use t1_v1_v2_uniq_partial_idx.
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 AND v3 > 2;
EXPLAIN SELECT * FROM t1 WHERE v1 = 1 AND v3 > 3;

--------------------------------------
-- Range-partitioned tables/indexes.
--------------------------------------

CREATE TABLE t2(h int, r int, v1 int, v2 int, v3 int, primary key(h ASC, r ASC));
CREATE INDEX t2_v1_v2_idx on t2(v1 ASC, v2 ASC);
CREATE UNIQUE INDEX t2_v3_uniq_idx on t2(v3 ASC);

-- Expect to use t2_v3_uniq_idx because will guarantee a single-row scan (due to uniqueness).
EXPLAIN SELECT * FROM t2 WHERE v1 = 1 and v2 = 1 and v3 = 1;

-- Expect to use t2_v1_v2_idx because condition partly matches.
EXPLAIN SELECT * FROM t2 WHERE v1 >= 1 and v2 > 1;

--------------------------------------
-- Test covered vs uncovered indexes.

-- Should prioritize the pkey index because it covers all columns (index only scan).
EXPLAIN SELECT * FROM t2 WHERE h = 1 and v1 = 1;
EXPLAIN SELECT * FROM t2 WHERE h = 1 and r = 2 and v1 = 1 and v2 = 2;

-- Should prioritize the t1_v1_v2_idx because it is fully specified.
EXPLAIN SELECT * FROM t2 WHERE h = 1 and v1 = 1 and v2 = 2;

--------------------------------------
-- Test partial indexes.

DROP INDEX t2_v3_uniq_idx;

-- Should use t1_v1_v2_idx because conditions partly match.
EXPLAIN SELECT * FROM t2 WHERE v1 = 1 AND v3 > 1;
EXPLAIN SELECT * FROM t2 WHERE v1 = 1 AND v3 > 2;
EXPLAIN SELECT * FROM t2 WHERE v1 = 1 AND v3 > 3;

CREATE INDEX t2_v1_v2_uniq_partial_idx ON t2(v1, v2) WHERE v3 > 2;

-- Should still use t1_v1_v2_idx because partial index condition does not match.
EXPLAIN SELECT * FROM t2 WHERE v1 = 1 AND v3 > 1;

-- Should both use t1_v1_v2_uniq_partial_idx.
EXPLAIN SELECT * FROM t2 WHERE v1 = 1 AND v3 > 2;
EXPLAIN SELECT * FROM t2 WHERE v1 = 1 AND v3 > 3;

--------------------------------------
-- Backwards vs forward scans.
--------------------------------------

CREATE TABLE t3(k int primary key, v1 int, v2 int);
CREATE INDEX asc_idx on t3(v1 HASH, v2 ASC);
CREATE INDEX desc_idx on t3(v1 HASH, v2 DESC);

-- Should use asc index.
EXPLAIN SELECT * FROM t3 WHERE v1 = 1 ORDER BY v2 ASC;
EXPLAIN SELECT * FROM t3 WHERE v1 = 1 AND v2 > 3 ORDER BY v2 ASC;
EXPLAIN SELECT * FROM t3 WHERE v1 = 1 AND v2 <= 3 ORDER BY v2 ASC;

-- Should use desc index.
EXPLAIN SELECT * FROM t3 WHERE v1 = 1 ORDER BY v2 DESC;
EXPLAIN SELECT * FROM t3 WHERE v1 = 1 AND v2 > 3 ORDER BY v2 DESC;
EXPLAIN SELECT * FROM t3 WHERE v1 = 1 AND v2 <= 3 ORDER BY v2 DESC;

DROP TABLE t1;
DROP TABLE t2;
DROP TABLE t3;
