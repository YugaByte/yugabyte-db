-- Check update operations on very large table doesn't cause OOM
CREATE TABLE test(k INT PRIMARY KEY, v1 TEXT, v2 TEXT);
INSERT INTO test SELECT i, md5(random()::text), md5(random()::text) from generate_series(1, 500000) AS i;
UPDATE test SET v1 = v2;
DELETE FROM test;
