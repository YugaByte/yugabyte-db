--
-- ALTER TABLE with table rewrite tests.
--
\getenv abs_srcdir PG_ABS_SRCDIR
\set filename :abs_srcdir '/yb_commands/parameterized_query.sql'
\i :filename
\set P1 ':explain'
\set P2

-- Suppress NOTICE messages during table rewrite operations.
SET client_min_messages TO WARNING;
-- Test add column operation with table rewrite.
CREATE TABLESPACE test_tablespace LOCATION '/invalid';
CREATE TABLE table_rewrite_test1_fk1(a int PRIMARY KEY);
INSERT INTO table_rewrite_test1_fk1 VALUES (1), (2), (3), (4);
CREATE TABLE table_rewrite_test1
    (a int REFERENCES table_rewrite_test1_fk1(a), b int, c text,
     d int generated always as identity, drop_me int, e int DEFAULT 10,
     f int NOT NULL, username text,
     PRIMARY KEY (a ASC))
    TABLESPACE test_tablespace;
ALTER TABLE table_rewrite_test1 DROP COLUMN drop_me;
INSERT INTO table_rewrite_test1 (a, b, c, e, f, username)
    VALUES(1, 1, 'text', 10, 1, 'user1'), (2, 2, 'text2', 20, 2, 'user2'),
    (3, 3, 'text33', 30, 3, 'user3');
-- create some dependencies on the table.
-- 1. check constraint.
ALTER TABLE table_rewrite_test1 ADD CONSTRAINT a_check CHECK(a > 0);
-- 2. indexes.
CREATE UNIQUE INDEX test_index ON table_rewrite_test1(b)
    TABLESPACE test_tablespace;
CREATE INDEX test_index2 ON table_rewrite_test1((e * 2))
    TABLESPACE test_tablespace;
-- 3. foreign key constraint.
CREATE TABLE table_rewrite_test1_fk2(a int REFERENCES table_rewrite_test1(b));
-- 4. dependent view.
CREATE VIEW table_rewrite_test1_view AS SELECT a, b FROM table_rewrite_test1;
-- 5. column with missing default value.
ALTER TABLE table_rewrite_test1 ADD COLUMN g int DEFAULT 5;
-- 6. dependent trigger.
CREATE FUNCTION dummy() RETURNS TRIGGER
AS $$
BEGIN
    RETURN NULL;
END;
$$ LANGUAGE PLPGSQL;
CREATE TRIGGER dummy_trigger
    AFTER INSERT
    ON table_rewrite_test1
    FOR EACH ROW
    EXECUTE PROCEDURE dummy();
-- 6. enable RLS and create policy objects.
ALTER TABLE table_rewrite_test1 ENABLE ROW LEVEL SECURITY;
CREATE USER user1;
CREATE POLICY p ON table_rewrite_test1
    FOR SELECT TO user1 USING (username = CURRENT_USER);
GRANT SELECT ON table_rewrite_test1 TO user1;
-- 7. extended statistics object.
CREATE STATISTICS s1(dependencies) ON a, b FROM table_rewrite_test1;
-- perform several table rewrite operations.
ALTER TABLE table_rewrite_test1 ADD COLUMN h SERIAL, ADD COLUMN i BIGSERIAL;
ALTER TABLE table_rewrite_test1
    ADD COLUMN j timestamp DEFAULT clock_timestamp() NOT NULL,
    DROP CONSTRAINT table_rewrite_test1_pkey;
ALTER TABLE table_rewrite_test1 ADD PRIMARY KEY(a DESC);
ALTER TABLE table_rewrite_test1 ALTER c TYPE int USING LENGTH(c);
-- verify table data.
SELECT a, b, username, c, d, e, f, g, h, i FROM table_rewrite_test1 ORDER BY a;
-- verify that we preserved dependent objects.
\d table_rewrite_test1;
\d test_index;
\d test_index2;
SELECT * FROM table_rewrite_test1_view ORDER BY a;
EXPLAIN SELECT a, b, username, c, d, e, f, g, h, i FROM table_rewrite_test1 WHERE b = 3;
SELECT a, b, username, c, d, e, f, g, h, i FROM table_rewrite_test1 WHERE b = 3;
EXPLAIN SELECT a, b, username, c, d, e, f, g, h, i FROM table_rewrite_test1 WHERE e * 2 = 20;
SELECT a, b, username, c, d, e, f, g, h, i FROM table_rewrite_test1 WHERE e * 2 = 20;
-- verify PK constraint.
INSERT INTO table_rewrite_test1(a, f) VALUES (3, 4); -- should fail.
-- verify FK constraint where the altered table references a FK table.
INSERT INTO table_rewrite_test1(a, f) VALUES (4, 4);
INSERT INTO table_rewrite_test1(a, f) VALUES (5, 5); -- should fail.
INSERT INTO table_rewrite_test1_fk1 VALUES (5), (6);
INSERT INTO table_rewrite_test1(a, f) VALUES (5, 5);
DELETE FROM table_rewrite_test1_fk1 WHERE a = 3; -- should fail.
-- verify FK constraint where another table references the altered table via
-- an FK constraint..
INSERT INTO table_rewrite_test1_fk2(a) VALUES (3);
INSERT INTO table_rewrite_test1_fk2(a) VALUES (6); -- should fail.
INSERT INTO table_rewrite_test1(a, b, f) VALUES (6, 4, 4);
INSERT INTO table_rewrite_test1_fk2(a) VALUES (4);
DELETE FROM table_rewrite_test1 WHERE b = 3; -- should fail.
DELETE FROM table_rewrite_test1_fk2;
DELETE FROM table_rewrite_test1 WHERE b = 3;
INSERT INTO table_rewrite_test1_fk2 VALUES (3); -- should fail.
-- verify policies and RLS.
SET ROLE user1;
SELECT a, b, username, c, d, e, f, g, h, i FROM table_rewrite_test1 ORDER BY a;
SET ROLE yugabyte;

-- verify future ALTER operations on the table succeed.
ALTER TABLE table_rewrite_test1 ADD COLUMN k int, DROP COLUMN b CASCADE;
ALTER TABLE table_rewrite_test1 RENAME TO table_rewrite_test1_new;

-- cleanup.
DROP TABLE table_rewrite_test1_new CASCADE;
DROP USER user1;

-- Test table rewrite operations on a partitioned table.
CREATE TABLE test_partitioned(a int, PRIMARY KEY (a ASC), b text) PARTITION BY RANGE (a);
CREATE TABLE test_partitioned_part1 PARTITION OF test_partitioned FOR VALUES FROM (1) TO (6);
CREATE TABLE test_partitioned_part2 PARTITION OF test_partitioned FOR VALUES FROM (6) TO (11);
CREATE INDEX test_partitioned_index ON test_partitioned(b);
INSERT INTO test_partitioned VALUES(generate_series(1, 10), 'text');
ALTER TABLE test_partitioned ADD COLUMN c SERIAL, ALTER COLUMN b TYPE int USING length(b);
ALTER TABLE test_partitioned DROP CONSTRAINT test_partitioned_pkey;
ALTER TABLE test_partitioned ADD PRIMARY KEY (a ASC);
SELECT * FROM test_partitioned;
EXPLAIN (COSTS OFF) SELECT * FROM test_partitioned WHERE b = 4;
SELECT * FROM test_partitioned WHERE b = 4;

-- Test table rewrite on temp tables.
CREATE TEMP TABLE temp_table_rewrite_test(a int);
INSERT INTO temp_table_rewrite_test VALUES(1), (2), (3);
CREATE INDEX ON temp_table_rewrite_test(a);
ALTER TABLE temp_table_rewrite_test ADD COLUMN c SERIAL;
SELECT * FROM temp_table_rewrite_test ORDER BY a;

-- Test rewrite with colocation.
CREATE DATABASE mydb WITH colocation = true;
\c mydb;
-- Suppress NOTICE messages during table rewrite operations.
SET client_min_messages TO WARNING;
CREATE TABLE base (col int, col2 int);
CREATE INDEX base_idx ON base(col2);
INSERT INTO base VALUES (1, 3), (2, 2), (3, 1);
ALTER TABLE base ADD PRIMARY KEY (col HASH); -- should fail.
ALTER TABLE base
    ADD PRIMARY KEY (col), ADD COLUMN col3 float DEFAULT random();
ALTER TABLE base ALTER COLUMN col2 TYPE int2;
ALTER TABLE base ADD COLUMN col4 SERIAL;
SELECT col, col2, col4 FROM base;
SELECT col, col2, col4 FROM base WHERE col2 = 1;
ALTER TABLE base DROP CONSTRAINT base_pkey;
SELECT col, col2, col4 FROM base ORDER BY col;
\d+ base;
SELECT num_tablets, num_hash_key_columns, is_colocated FROM
    yb_table_properties('base_idx'::regclass);
CREATE UNIQUE INDEX base_idx_unique ON base(col);
ALTER TABLE base ADD PRIMARY KEY USING INDEX base_idx_unique;
INSERT INTO base VALUES (1, 1); -- should fail.
INSERT INTO base VALUES (4, 4), (5, 5);
SELECT col, col2 FROM base;
CREATE TABLE base2 (col int, col2 int) WITH (COLOCATION=false);
CREATE INDEX base2_idx ON base2(col2);
INSERT INTO base2 VALUES (1, 3), (2, 2), (3, 1);
ALTER TABLE base2
    ADD PRIMARY KEY (col ASC), ADD COLUMN col3 float DEFAULT random();
ALTER TABLE base2 ALTER COLUMN col2 TYPE int2;
ALTER TABLE base2 ADD COLUMN col4 SERIAL;
SELECT col, col2, col4 FROM base2;
SELECT col, col2, col4 FROM base2 WHERE col2 = 1;
ALTER TABLE base2 DROP CONSTRAINT base2_pkey;
SELECT col, col2, col4 FROM base2 ORDER BY col;
\d+ base2;
SELECT num_tablets, num_hash_key_columns, is_colocated FROM
    yb_table_properties('base2_idx'::regclass);
CREATE TABLE t5(c0 money UNIQUE);
ALTER TABLE ONLY t5 ALTER COLUMN c0 SET DATA TYPE money USING c0::text::money;
\d t5;
\c yugabyte;
-- Suppress NOTICE messages during table rewrite operations.
SET client_min_messages TO WARNING;
-- Test rewrite with tablegroups.
CREATE TABLEGROUP tg1;
CREATE TABLE test_tablegroup (id int) TABLEGROUP tg1;
INSERT INTO test_tablegroup VALUES (1), (2);
CREATE TABLE test_tablegroup2 (id int, id2 int UNIQUE WITH
    (colocation_id=100501)) WITH (colocation_id=100500) TABLEGROUP tg1;
INSERT INTO test_tablegroup2 VALUES (1, 1), (2, 2);
ALTER TABLE test_tablegroup ADD PRIMARY KEY (id ASC);
ALTER TABLE test_tablegroup ADD COLUMN c SERIAL;
ALTER TABLE test_tablegroup2 ADD PRIMARY KEY (id ASC);
ALTER TABLE test_tablegroup2 ADD COLUMN c SERIAL;
SELECT * FROM test_tablegroup;
SELECT * FROM test_tablegroup2;
\d test_tablegroup;
\d test_tablegroup2;
ALTER TABLE test_tablegroup DROP CONSTRAINT test_tablegroup_pkey;
ALTER TABLE test_tablegroup2 DROP CONSTRAINT test_tablegroup2_pkey;
SELECT * FROM test_tablegroup ORDER BY id;
SELECT * FROM test_tablegroup2 ORDER BY id;
\d test_tablegroup;
\d test_tablegroup2;

-- Tests for ALTER TABLE ... ADD COLUMN rewrite operations.
-- verify split options are preserved.
CREATE TABLE test_add_column(a int, b int)
    SPLIT INTO 2 TABLETS;
CREATE INDEX test_add_column_idx ON test_add_column(b ASC)
    SPLIT AT VALUES ((5), (10));
INSERT INTO test_add_column VALUES (1, 1);
ALTER TABLE test_add_column ADD COLUMN c SERIAL;
ALTER TABLE test_add_column ADD COLUMN d SERIAL PRIMARY KEY;
SELECT num_tablets, num_hash_key_columns FROM
    yb_table_properties('test_add_column'::regclass);
SELECT yb_get_range_split_clause('test_add_column_idx'::regclass);
INSERT INTO test_add_column(a, d) VALUES (2, 1); -- should fail.
INSERT INTO test_add_column(a, d) VALUES (2, 2);
SELECT * FROM test_add_column;

-- Tests for ALTER TABLE ADD/DROP PRIMARY KEY.
-- basic tests.
CREATE TABLE nopk (id int, v int);
ALTER TABLE nopk ADD PRIMARY KEY (id);
ALTER TABLE nopk ADD PRIMARY KEY (id); -- should fail.
ALTER TABLE nopk DROP CONSTRAINT nopk_pkey;
INSERT INTO nopk VALUES (1, 1);
INSERT INTO nopk VALUES (1, 2);
ALTER TABLE nopk ADD PRIMARY KEY (id); -- should fail.
DELETE FROM nopk WHERE v = 2;
ALTER TABLE nopk ADD PRIMARY KEY (id);
ALTER TABLE nopk DROP CONSTRAINT nopk_pkey;
ALTER TABLE nopk ALTER COLUMN id DROP NOT NULL;
INSERT INTO nopk VALUES (null);
ALTER TABLE nopk ADD PRIMARY KEY (id); -- should fail.
DROP TABLE nopk;
-- test complex pks.
CREATE TABLE complex_pk (v1 int, v2 text, v3 char, v4 boolean);
INSERT INTO complex_pk VALUES (1, '111', '1', 'true'), (2, '222', '2', 'false');
ALTER TABLE complex_pk ADD PRIMARY KEY ((v1, v2) HASH, v3 ASC, v4 DESC);
SELECT * FROM yb_table_properties('complex_pk'::regclass);
INSERT INTO complex_pk VALUES (2, '222', '3', 'true');
INSERT INTO complex_pk VALUES (2, '222', '2', 'false'); -- should fail.
SELECT * FROM complex_pk ORDER BY v1, v2, v3, v4;
ALTER TABLE complex_pk DROP CONSTRAINT complex_pk_pkey;
INSERT INTO complex_pk VALUES (2, '222', '3', 'true');
SELECT * FROM complex_pk ORDER BY v1, v2, v3, v4;
-- test range pks.
CREATE TABLE range_pk (id int);
INSERT INTO range_pk VALUES (1), (2), (3);
ALTER TABLE range_pk ADD PRIMARY KEY (id DESC);
SELECT * FROM range_pk; -- should be in descending order.
ALTER TABLE range_pk DROP CONSTRAINT range_pk_pkey;
SELECT * FROM range_pk ORDER BY id;
-- test include pks.
CREATE TABLE include_pk (id int, v1 int, v2 int);
INSERT INTO include_pk VALUES (1, 11, 111), (2, 22, 222);
ALTER TABLE include_pk ADD PRIMARY KEY (id) INCLUDE (v1, v2);
INSERT INTO include_pk VALUES (3, 11, 111);
EXPLAIN SELECT v1 FROM include_pk WHERE id = 2;
SELECT v1 FROM include_pk WHERE id = 2;
INSERT INTO include_pk VALUES (3, 99, 999); -- should fail.
SELECT * FROM include_pk ORDER BY id;
EXPLAIN SELECT v1 FROM include_pk WHERE id = 3;
SELECT v1 FROM include_pk WHERE id = 3;
ALTER TABLE include_pk DROP CONSTRAINT include_pk_pkey;
EXPLAIN SELECT v1 FROM include_pk WHERE id = 3;
SELECT v1 FROM include_pk WHERE id = 3;
-- test pk with UDT.
CREATE TYPE typeid AS (i int);
CREATE TABLE nopk_udt (id typeid, v int);
ALTER TABLE nopk_udt ADD PRIMARY KEY (id); -- should fail.
-- test pk USING INDEX.
CREATE TABLE nopk_usingindex (id int) SPLIT INTO 5 TABLETS;
INSERT INTO nopk_usingindex VALUES (1), (2), (3);
CREATE INDEX nopk_idx ON nopk_usingindex(id);
CREATE UNIQUE INDEX nopk_idx2 ON nopk_usingindex (id HASH);
CREATE UNIQUE INDEX nopk_idx3 ON nopk_usingindex (id ASC);
CREATE UNIQUE INDEX nopk_idx4 ON nopk_usingindex (id DESC);
ALTER TABLE nopk_usingindex ADD PRIMARY KEY USING INDEX nopk_idx; -- should fail.
INSERT INTO nopk_usingindex VALUES (null);
ALTER TABLE nopk_usingindex ADD PRIMARY KEY USING INDEX nopk_idx2; -- should fail.
DELETE FROM nopk_usingindex WHERE id IS NULL;
ALTER TABLE nopk_usingindex ADD PRIMARY KEY USING INDEX nopk_idx2;
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('nopk_usingindex'::regclass);
INSERT INTO nopk_usingindex VALUES (4);
INSERT INTO nopk_usingindex VALUES (1); -- should fail.
INSERT INTO nopk_usingindex VALUES (null); -- should fail.
SELECT * FROM nopk_usingindex ORDER BY id;
DROP INDEX nopk_idx2; -- should fail.
ALTER TABLE nopk_usingindex DROP CONSTRAINT nopk_idx2;
ALTER TABLE nopk_usingindex ADD PRIMARY KEY USING INDEX nopk_idx3;
SELECT * FROM nopk_usingindex;
DROP INDEX nopk_idx3; -- should fail.
ALTER TABLE nopk_usingindex DROP CONSTRAINT nopk_idx3;
ALTER TABLE nopk_usingindex ADD PRIMARY KEY USING INDEX nopk_idx4; -- should fail (not supported in PG).
-- test adding/dropping pks on partitioned tables.
CREATE TABLE nopk_whole (id int) PARTITION BY LIST (id);
CREATE TABLE nopk_part1 PARTITION OF nopk_whole FOR VALUES IN (1, 2, 3);
CREATE TABLE nopk_part2 PARTITION OF nopk_whole FOR VALUES IN (10, 20, 30, 40)
    PARTITION BY LIST (id);
CREATE TABLE nopk_part2_part1 PARTITION OF nopk_part2 FOR VALUES IN (10, 20);
CREATE TABLE nopk_part2_part2 PARTITION OF nopk_part2 FOR VALUES IN (30, 40);
ALTER TABLE nopk_whole ADD PRIMARY KEY (id);
-- verify that we cannot drop inherited PK constraints.
ALTER TABLE nopk_part1 DROP CONSTRAINT nopk_part1_pkey; -- should fail.
ALTER TABLE nopk_part2 DROP CONSTRAINT nopk_part2_pkey; -- should fail.
ALTER TABLE nopk_part2_part1 DROP CONSTRAINT nopk_part2_part1_pkey; -- should fail.
ALTER TABLE nopk_part2_part2 DROP CONSTRAINT nopk_part2_part2_pkey; -- should fail.
ALTER TABLE nopk_whole DROP CONSTRAINT nopk_whole_pkey;
-- verify that we can successfully add and drop PK constraints on partitions.
ALTER TABLE nopk_part1 ADD PRIMARY KEY (id);
ALTER TABLE nopk_part1 DROP CONSTRAINT nopk_part1_pkey;
ALTER TABLE nopk_part2 ADD PRIMARY KEY (id);
ALTER TABLE nopk_part2 DROP CONSTRAINT nopk_part2_pkey;
ALTER TABLE nopk_part2_part1 ADD PRIMARY KEY (id);
ALTER TABLE nopk_part2_part1 DROP CONSTRAINT nopk_part2_part1_pkey;
ALTER TABLE nopk_part2_part2 ADD PRIMARY KEY (id);
ALTER TABLE nopk_part2_part2 DROP CONSTRAINT nopk_part2_part2_pkey;
-- tests for altered table referenced by a partitioned FK table.
CREATE TABLE test (id int unique);
CREATE TABLE test_part (id int REFERENCES test(id)) PARTITION BY RANGE(id);
CREATE TABLE test_part_1 PARTITION OF test_part FOR VALUES FROM (1) TO (100);
INSERT INTO test VALUES (1);
INSERT INTO test_part VALUES (1);
ALTER TABLE test ADD PRIMARY KEY (id);
SELECT * FROM test;
SELECT * FROM test_part ORDER BY id;
INSERT INTO test_part VALUES (2); -- should fail.
INSERT INTO test_part_1 VALUES (2); -- should fail.
DROP TABLE test CASCADE;
-- tests for split options.
-- verify split options are preserved when we add/drop a hash key.
CREATE TABLE nopk (id int, a int, b text, c float, d timestamp, e money)
    SPLIT INTO 5 TABLETS;
CREATE INDEX nopk_idx1 ON nopk(id) SPLIT INTO 4 TABLETS;
CREATE INDEX nopk_idx2 ON nopk(id ASC, a ASC, b ASC, c ASC, d ASC, e ASC)
    SPLIT AT VALUES
    ((10, 20, E'test123\"\"''\\\\\\u0068\\u0069', '-Infinity', '1999-01-01',
    '12.34'),
    (20, 30, E'test123\"\"''\\\\\\u0068\\u0069z', 'Infinity', '2023-01-01',
    '56.78'));
ALTER TABLE nopk ADD PRIMARY KEY (id);
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('nopk'::regclass);
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('nopk_idx1'::regclass);
SELECT yb_get_range_split_clause('nopk_idx2'::regclass);
ALTER TABLE nopk DROP CONSTRAINT nopk_pkey;
-- verify split options are not preserved when we add a range key, and only the number of
-- tablets is preserved when we drop a range key.
CREATE TABLE nopk2 (id int) SPLIT INTO 5 TABLETS;
ALTER TABLE nopk2 ADD PRIMARY KEY (id);
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('nopk2'::regclass);
CREATE TABLE range_pk_test (id int, primary key(id asc)) SPLIT AT VALUES ((5), (10), (15), (20));
ALTER TABLE range_pk_test DROP CONSTRAINT range_pk_test_pkey;
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('range_pk_test'::regclass);
-- rules:
-- 1. new table rewrite + rule
-- 2. old table rewrite + rule -- fail
-- 3. new table rewrite + no more rule
-- 4. old table rewrite + no more rule
CREATE TABLE pk_rule_table (id int);
CREATE RULE pk_rule AS
    ON INSERT TO pk_rule_table
    DO INSTEAD DELETE FROM pk_rule_table;
ALTER TABLE pk_rule_table ADD PRIMARY KEY (id);
SET yb_enable_alter_table_rewrite = OFF;
ALTER TABLE pk_rule_table DROP CONSTRAINT pk_rule_table_pkey; -- should fail.
RESET yb_enable_alter_table_rewrite;
DROP RULE pk_rule ON pk_rule_table;
ALTER TABLE pk_rule_table DROP CONSTRAINT pk_rule_table_pkey;
SET yb_enable_alter_table_rewrite = OFF;
ALTER TABLE pk_rule_table ADD PRIMARY KEY (id);
RESET yb_enable_alter_table_rewrite;

-- Tests for ALTER TYPE.
-- basic tests.
CREATE TABLE varchar_table(id SERIAL, c1 varchar(10), PRIMARY KEY (id ASC));
CREATE TABLE int4_table(id SERIAL, c1 int4, PRIMARY KEY (id ASC));
ALTER TABLE varchar_table ALTER c1 TYPE int; -- should fail.
ALTER TABLE int4_table ALTER c1 TYPE int8;
INSERT INTO int4_table(c1) VALUES (2 ^ 40);
ALTER TABLE int4_table ALTER c1 TYPE int4; -- should fail.
INSERT INTO varchar_table(c1) VALUES ('aa');
ALTER TABLE varchar_table ALTER c1 TYPE varchar(1); -- should fail.
INSERT INTO varchar_table(c1) VALUES ('a'), (2), ('aaa');
ALTER TABLE varchar_table ALTER c1 TYPE text;
SELECT * from varchar_table;
INSERT INTO varchar_table(c1) VALUES ('a'), ('bb'), ('ccc');
ALTER TABLE varchar_table ALTER c1 TYPE int USING length(c1);
SELECT * from varchar_table;
ALTER TABLE varchar_table ALTER c1 TYPE double precision USING sqrt(c1);
SELECT * from varchar_table;
ALTER TABLE int4_table ADD COLUMN c2 varchar, ADD COLUMN c3 int, ALTER c1 TYPE varchar;
INSERT INTO int4_table(c1, c2, c3) VALUES ('a', 'a', 1), ('b', 'b', 2), ('c', 'c', 3);
SELECT * from int4_table;
CREATE TABLE int4_table2(c1 int) PARTITION BY RANGE(c1);
ALTER TABLE int4_table2 ALTER c1 TYPE varchar; -- should fail.
CREATE TABLE pk_table(c1 int primary key);
INSERT INTO pk_table VALUES (1), (2);
ALTER TABLE pk_table ALTER c1 TYPE varchar;
SELECT * from pk_table ORDER BY c1 ASC;
INSERT INTO pk_table VALUES (1); -- should fail.
ALTER TABLE pk_table ALTER c1 TYPE int USING length(c1);  -- should fail.
-- test ALTER TYPE on a column that is a part of a view/rule:
-- 1. column part of view --fail
-- 2. column not part of view
-- 3. column part of view via _RETURN rule -- fail
-- 4. column not part of view via _RETURN rule
-- 5. rule on other table/view that affects this table
-- 6. rule on other table/view + rule on this table
-- 7. rule on other table/view + rule on this table + old rewrite -- fail
-- 8. rule on other table/view + no more rule on this table
-- 9. rule on other table/view (this is dropped for #22063) + no more rule on this table + old rewrite
CREATE TABLE test_table (c1 int, c2 varchar, c3 varchar, c4 varchar);
CREATE VIEW test_view AS SELECT c2 FROM test_table;
ALTER TABLE test_table ALTER c2 TYPE int USING length(c2); -- should fail.
ALTER TABLE test_table ALTER c3 TYPE varchar(1);
CREATE TABLE dummy_table (c3 varchar);
CREATE RULE "_RETURN" AS
    ON SELECT TO dummy_table
    DO INSTEAD
        SELECT c3 FROM test_table;
ALTER TABLE test_table ALTER c3 TYPE varchar(1); -- should fail.
ALTER TABLE test_table ALTER c4 TYPE varchar(1);
CREATE RULE dummy_rule AS
    ON INSERT TO dummy_table
    DO INSTEAD DELETE FROM test_table;
ALTER TABLE test_table ALTER c4 TYPE char;
CREATE RULE test_rule AS
    ON INSERT TO test_table
    DO INSTEAD DELETE FROM dummy_table;
ALTER TABLE test_table ALTER c4 TYPE varchar(1);
SET yb_enable_alter_table_rewrite = OFF;
ALTER TABLE test_table ALTER c4 TYPE char; -- should fail.
RESET yb_enable_alter_table_rewrite;
DROP RULE test_rule ON test_table;
ALTER TABLE test_table ALTER c4 TYPE char;
SET yb_enable_alter_table_rewrite = OFF;
-- TODO(#22063): DROP VIEW is needed to avoid non-deterministic error.
DROP VIEW dummy_table;
ALTER TABLE test_table ALTER c4 TYPE varchar(1);
RESET yb_enable_alter_table_rewrite;
DROP TABLE test_table CASCADE;
-- test ALTER TYPE on a foreign key column.
CREATE TABLE fk_table (c1 varchar primary key);
CREATE TABLE test_table (c1 varchar primary key references fk_table(c1));
CREATE TABLE fk_table2 (c1 varchar references test_table(c1));
INSERT INTO fk_table VALUES ('a'), ('aa'), ('aaa');
INSERT INTO test_table VALUES ('a'), ('aa'), ('aaa');
INSERT INTO fk_table2 VALUES ('a'), ('aa'), ('aaa');
ALTER TABLE fk_table ALTER c1 TYPE int USING LENGTH(c1); -- should fail.
ALTER TABLE fk_table ALTER c1 TYPE varchar(3);
\d fk_table;
\d test_table;
\d fk_table2;
INSERT INTO fk_table2 VALUES ('aaaa'); -- should fail.
INSERT INTO test_table VALUES ('aaaa'); -- should fail.
DROP TABLE test_table CASCADE;
-- test ALTER TYPE where an index with a default tablespace is re-created.
CREATE TABLESPACE test_tblspc LOCATION '/invalid';
SET default_tablespace = test_tblspc;
CREATE TABLE test_table (c1 int, c2 int);
CREATE INDEX test_index ON test_table(c1);
ALTER TABLE test_table ALTER c1 TYPE int2;
\d test_index;
-- test ALTER TYPE on a table with a range key.
CREATE TABLE range_key_table(col1 int, a int, b int, col2 int, c text, d text,
    col3 int, e int, PRIMARY KEY((a, b) HASH, c ASC, d DESC) INCLUDE (e));
INSERT INTO range_key_table(a, b, c, d)
    VALUES (1, 1, 'a', 'e'), (1, 1, 'aa', 'f'), (1, 1, 'aa', 'g');
CREATE INDEX ON range_key_table(c ASC);
ALTER TABLE range_key_table ALTER c TYPE int USING length(c);
SELECT a, b, c, d FROM range_key_table;
\d range_key_table;
-- tests for split options.
CREATE TABLE test1 (c1 varchar, c2 varchar) SPLIT INTO 5 TABLETS;
CREATE INDEX idx1 ON test1(c1) SPLIT INTO 5 TABLETS;
CREATE INDEX idx2 ON test1(c1 HASH, c2 ASC) SPLIT INTO 5 TABLETS;
CREATE INDEX idx3 ON test1(c2 HASH) INCLUDE(c1) SPLIT INTO 5 TABLETS;
CREATE INDEX idx4 ON test1(c1 ASC) SPLIT AT VALUES (('h'));
ALTER TABLE test1 ALTER c1 TYPE int USING length(c1);
-- verify hash split options on the table are preserved.
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('test1'::regclass);
-- verify hash split options on the indexes are preserved.
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('idx1'::regclass);
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('idx2'::regclass);
SELECT num_tablets, num_hash_key_columns FROM yb_table_properties('idx3'::regclass);
SELECT yb_get_range_split_clause('idx4'::regclass);
CREATE TABLE test2 (c1 varchar, c2 varchar, PRIMARY KEY(c1 ASC, c2 DESC))
    SPLIT AT VALUES (('h', 20));
ALTER TABLE test2 ALTER c1 TYPE int USING length(c1);
CREATE TABLE test3 (c1 varchar, c2 varchar, PRIMARY KEY(c2 ASC))
    SPLIT AT VALUES ((E'test123\"\"''\\\\\\u0068\\u0069'));
ALTER TABLE test3 ALTER c1 TYPE int USING length(c1);
-- verify range split options on the table are only preserved when the altered column is not a
-- part of the table's key.
SELECT yb_get_range_split_clause('test2'::regclass);
SELECT yb_get_range_split_clause('test3'::regclass);
-- test ALTER TYPE on temp tables.
CREATE TEMP TABLE test_table (col1 text UNIQUE);
INSERT INTO test_table VALUES ('1'), ('01');
ALTER TABLE test_table ALTER COLUMN col1 TYPE integer using col1::int;
ALTER TABLE test_table DROP CONSTRAINT test_table_col1_key;
ALTER TABLE test_table ALTER COLUMN col1 TYPE integer using col1::int;
SELECT * FROM test_table;

--
-- Test ALTER TABLE ... ADD COLUMN ... GENERATED ALWAYS AS IDENTITY
--
CREATE TABLE test_identity (id int, PRIMARY KEY (id ASC));
INSERT INTO test_identity VALUES (1), (2), (3);
ALTER TABLE test_identity ADD COLUMN id2 int GENERATED ALWAYS AS IDENTITY;
INSERT INTO test_identity VALUES (4, 4); -- should fail
INSERT INTO test_identity OVERRIDING SYSTEM VALUE VALUES (4, 4);
SELECT * FROM test_identity;

--
-- Test ALTER TABLE ... DROP COLUMN on a primary key column.
--
-- basic test
CREATE TABLE test_drop_pk_column (id int PRIMARY KEY, v int);
INSERT INTO test_drop_pk_column VALUES (1, 1), (2, 2), (3, 3);
ALTER TABLE test_drop_pk_column DROP COLUMN id;
\d test_drop_pk_column;
INSERT INTO test_drop_pk_column VALUES (1);
SELECT * FROM test_drop_pk_column ORDER BY v;
-- test composite PK
CREATE TABLE test_drop_pk_column_composite (id int, v int, t int, PRIMARY KEY (id HASH, v ASC));
INSERT INTO test_drop_pk_column_composite VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3);
ALTER TABLE test_drop_pk_column_composite DROP COLUMN id;
\d test_drop_pk_column_composite;
INSERT INTO test_drop_pk_column_composite VALUES (1, 1);
SELECT * FROM test_drop_pk_column_composite ORDER BY v;
-- test partitioned table
CREATE TABLE test_drop_pk_column_part (id int PRIMARY KEY, v int) PARTITION BY RANGE (id);
CREATE TABLE test_drop_pk_column_part1 PARTITION OF test_drop_pk_column_part FOR VALUES FROM (1) TO (10);
ALTER TABLE test_drop_pk_column_part DROP COLUMN id; -- should fail.
-- test rewrite on child table with a primary key column.
CREATE TABLE parent(a int, c int) PARTITION BY RANGE(a);
CREATE TABLE child(a int, c int, primary key (c));
ALTER TABLE parent ATTACH PARTITION child FOR VALUES FROM (1) TO (100);
ALTER TABLE parent DROP COLUMN c;
INSERT INTO parent VALUES (1);
SELECT * FROM parent;

--
-- Test index rewrites during ALTER TYPE when the base table is not rewritten.
--
-- test that indexes are correctly rebuilt when collation changes.
-- without rebuilding, queries can return incorrect results.
CREATE TABLE collation_test (name varchar COLLATE "en_US");
CREATE INDEX idx_collation ON collation_test(name ASC);
-- insert test data with special characters
INSERT INTO collation_test VALUES ('abc'), (U&'\00E4bc'), ('xyz');
-- query before collation change
SET enable_seqscan = off;
SET enable_sort = off;
-- should return: xyz
SELECT encode(name::bytea, 'escape') as utf8_bytes
    FROM collation_test WHERE name > 'x';
-- store the table's relfilenode before ALTER TYPE
CREATE TEMP TABLE table_info AS
    SELECT relfilenode FROM pg_class WHERE relname = 'collation_test';
-- store the index's relfilenode and OID before ALTER TYPE
CREATE TEMP TABLE index_info AS
    SELECT relfilenode, oid FROM pg_class WHERE relname = 'idx_collation';
-- change collation (no table rewrite needed)
ALTER TABLE collation_test ALTER COLUMN name TYPE varchar COLLATE "C";
-- check if table was rewritten (relfilenode changed) -- we expect it to not be rewritten
SELECT
    CASE
        WHEN old.relfilenode = new.relfilenode
        THEN 'Table not rewritten'
        ELSE 'Table rewritten'
    END as result
FROM table_info old, pg_class new
WHERE new.relname = 'collation_test';
-- check if index was rebuilt -- we expect it to be rebuilt (new oid and relfilenode)
SELECT
    CASE WHEN old.relfilenode = new.relfilenode THEN 'relfilenode reused' ELSE 'relfilenode changed' END,
    CASE WHEN old.oid = new.oid THEN 'OID reused' ELSE 'OID changed' END
FROM index_info old, pg_class new
WHERE new.relname = 'idx_collation';
-- query after collation change should return correct results
-- should return: xyz and 'U&'\00E4bc''
\set explain 'EXPLAIN (COSTS OFF)'
\set Q1 '/*+ IndexScan(collation_test idx_collation) */'
\set Q2 '/*+ SeqScan(collation_test) */'
SELECT $$
:P :Q
SELECT encode(name::bytea, 'escape') as utf8_bytes
FROM collation_test WHERE name > 'x' ORDER BY name;
$$ AS query \gset
\i :run_query
DROP TABLE table_info;
DROP TABLE index_info;
RESET enable_seqscan;
RESET enable_sort;

-- test that indexes are NOT unnecessarily rebuilt when compatible.
CREATE TABLE compatible_test (val varchar(10));
CREATE INDEX idx_compatible ON compatible_test(val);
INSERT INTO compatible_test VALUES ('abc'), ('def');
-- store index relfilenode, OID, and atttypmod before ALTER TYPE.
CREATE TEMP TABLE index_info AS
    SELECT c.relfilenode, c.oid, a.atttypmod
    FROM pg_class c, pg_attribute a
    WHERE c.relname = 'idx_compatible' AND a.attrelid = c.oid AND a.attnum = 1;
ALTER TABLE compatible_test ALTER COLUMN val TYPE varchar(100);
-- index should be reused: same relfilenode, same OID, but atttypmod updated.
SELECT
    CASE WHEN old.relfilenode = new.relfilenode THEN 'relfilenode reused' ELSE 'relfilenode changed' END,
    CASE WHEN old.oid = new.oid THEN 'OID reused' ELSE 'OID changed' END,
    CASE WHEN old.atttypmod <> a.atttypmod THEN 'atttypmod updated' ELSE 'atttypmod unchanged' END
FROM index_info old, pg_class new, pg_attribute a
WHERE new.relname = 'idx_compatible' AND a.attrelid = new.oid AND a.attnum = 1;
-- index scan should return correct results after in-place update.
SET enable_seqscan = off;
SELECT * FROM compatible_test WHERE val = 'abc';
RESET enable_seqscan;
-- test duplicate insert into unique index after ALTER TYPE without rewrite.
CREATE TABLE test (t varchar(10));
CREATE UNIQUE INDEX ON test(t);
INSERT INTO test VALUES ('hi');
ALTER TABLE test ALTER COLUMN t TYPE varchar(20);
INSERT INTO test VALUES ('hi');

-- test unique constraint, multi-column index, expression index, and mixed
-- compatible/incompatible in a single table.
CREATE TABLE inplace_test (a varchar(10) UNIQUE, b varchar(10) COLLATE "en_US");
CREATE INDEX idx_inplace_ab ON inplace_test(a, b);
CREATE INDEX idx_inplace_expr ON inplace_test(lower(b));
INSERT INTO inplace_test VALUES ('x', 'y');
CREATE TEMP TABLE inplace_info AS
    SELECT c.relname, c.oid, c.relfilenode FROM pg_class c
    WHERE c.relname IN ('inplace_test_a_key', 'idx_inplace_ab', 'idx_inplace_expr');
CREATE TEMP TABLE con_info AS
    SELECT oid, conindid FROM pg_constraint WHERE conname = 'inplace_test_a_key';
CREATE TEMP TABLE ab_attr AS
    SELECT attnum, atttypmod FROM pg_attribute
    WHERE attrelid = 'idx_inplace_ab'::regclass AND attnum > 0;
-- compatible alter on a, incompatible (collation change) on b.
ALTER TABLE inplace_test ALTER COLUMN a TYPE varchar(50),
    ALTER COLUMN b TYPE varchar(50) COLLATE "C";
-- unique constraint on a: OID and backing index should be reused.
SELECT
    CASE
        WHEN old.oid = new.oid AND old.conindid = new.conindid
        THEN 'constraint reused'
        ELSE 'constraint changed'
    END as result
FROM con_info old, pg_constraint new
WHERE new.conname = 'inplace_test_a_key';
-- unique constraint is still enforced.
INSERT INTO inplace_test VALUES ('x', 'z');
-- unique constraint index reused, expression index rebuilt.
SELECT m.relname,
    CASE WHEN m.relfilenode = new.relfilenode THEN 'relfilenode reused' ELSE 'relfilenode changed' END,
    CASE WHEN m.oid = new.oid THEN 'OID reused' ELSE 'OID changed' END
FROM inplace_info m
JOIN pg_class new ON new.relname = m.relname
ORDER BY m.relname;
-- multi-column index: column a's atttypmod changed, column b's changed too
-- (b was incompatible so the whole index was rebuilt).
SELECT m.attnum,
    CASE WHEN m.atttypmod = new.atttypmod THEN 'unchanged' ELSE 'changed' END as result
FROM ab_attr m
JOIN pg_attribute new ON new.attrelid = 'idx_inplace_ab'::regclass AND new.attnum = m.attnum
ORDER BY m.attnum;

-- temp table: follows PG behavior. Compatible index is reused, incompatible
-- index is rebuilt.
CREATE TEMP TABLE temp_test (a varchar(10), b varchar(10) COLLATE "en_US");
CREATE INDEX idx_temp_a ON temp_test(a);
CREATE INDEX idx_temp_b ON temp_test(b);
CREATE TEMP TABLE temp_info AS
    SELECT relname, oid, relfilenode FROM pg_class WHERE relname IN ('idx_temp_a', 'idx_temp_b');
ALTER TABLE temp_test ALTER COLUMN a TYPE varchar(50),
    ALTER COLUMN b TYPE varchar(50) COLLATE "C";
SELECT o.relname,
    CASE WHEN o.relfilenode = new.relfilenode THEN 'relfilenode reused' ELSE 'relfilenode changed' END,
    CASE WHEN o.oid = new.oid THEN 'OID reused' ELSE 'OID changed' END
FROM temp_info o
JOIN pg_class new ON new.relname = o.relname
ORDER BY o.relname;

-- partitioned table: compatible index reused in place for parent and children.
-- child partition has columns in different order to test attnum mapping.
CREATE TABLE part_parent (a varchar(10), b int) PARTITION BY RANGE (b);
CREATE TABLE part_child_raw (b int, a varchar(10));
ALTER TABLE part_parent ATTACH PARTITION part_child_raw FOR VALUES FROM (1) TO (100);
CREATE INDEX idx_part_a ON part_parent(a);
INSERT INTO part_parent VALUES ('x', 1);
CREATE TEMP TABLE part_idx_info AS
    SELECT c.relname, c.oid, c.relfilenode
    FROM pg_class c, pg_index i
    WHERE i.indexrelid = c.oid
      AND i.indrelid IN ('part_parent'::regclass, 'part_child_raw'::regclass);
ALTER TABLE part_parent ALTER COLUMN a TYPE varchar(50);
-- both parent and child index should be reused.
SELECT m.relname,
    CASE WHEN m.relfilenode = new.relfilenode THEN 'relfilenode reused' ELSE 'relfilenode changed' END,
    CASE WHEN m.oid = new.oid THEN 'OID reused' ELSE 'OID changed' END
FROM part_idx_info m
JOIN pg_class new ON new.relname = m.relname
ORDER BY m.relname;

-- inherited table (non-partition): compatible index reused in place.
CREATE TABLE inh_parent (a varchar(10));
CREATE TABLE inh_child () INHERITS (inh_parent);
CREATE INDEX idx_inh_parent ON inh_parent(a);
CREATE INDEX idx_inh_child ON inh_child(a);
CREATE TEMP TABLE inh_idx_info AS
    SELECT c.relname, c.oid, c.relfilenode
    FROM pg_class c
    WHERE c.relname IN ('idx_inh_parent', 'idx_inh_child');
ALTER TABLE inh_parent ALTER COLUMN a TYPE varchar(50);
-- both parent and child index should be reused.
SELECT m.relname,
    CASE WHEN m.relfilenode = new.relfilenode THEN 'relfilenode reused' ELSE 'relfilenode changed' END,
    CASE WHEN m.oid = new.oid THEN 'OID reused' ELSE 'OID changed' END
FROM inh_idx_info m
JOIN pg_class new ON new.relname = m.relname
ORDER BY m.relname;

--
-- Test ALTER TYPE with yb_enable_alter_table_rewrite = OFF when no table
-- rewrite is required (issue #32235). The legacy path used to skip
-- ATPostAlterTypeCleanup entirely, leaving dependent indexes pointing at the
-- old type's operator class; the stale operator class then made the YSQL
-- major version upgrade's pg_restore fail with "operator class ... does not
-- accept data type ...".
--
SET yb_enable_alter_table_rewrite = OFF;
-- timestamp -> timestamptz requires no table rewrite only under UTC.
SET timezone = 'UTC';
CREATE TABLE legacy_alter_type_test (id int, id2 int, created_at timestamp,
    PRIMARY KEY (id ASC));
CREATE INDEX legacy_alter_type_idx ON legacy_alter_type_test (id2 ASC, created_at DESC);
INSERT INTO legacy_alter_type_test
    SELECT g, g, '2024-01-01 00:00:00'::timestamp + make_interval(mins => g)
    FROM generate_series(1, 5) g;
CREATE TEMP TABLE legacy_alter_type_info AS
    SELECT oid, relfilenode FROM pg_class WHERE relname = 'legacy_alter_type_test';
ALTER TABLE legacy_alter_type_test ALTER COLUMN created_at TYPE timestamptz;
-- the no-rewrite type change must not have rewritten the table.
SELECT CASE WHEN old.oid = new.oid AND old.relfilenode = new.relfilenode
        THEN 'table not rewritten' ELSE 'table rewritten' END AS result
    FROM legacy_alter_type_info old, pg_class new
    WHERE new.relname = 'legacy_alter_type_test';
-- the index must use the new type's default operator class, so
-- pg_get_indexdef must not print an explicit operator class.
SELECT pg_get_indexdef('legacy_alter_type_idx'::regclass);
-- indclass and the index's pg_attribute rows must match the new column type.
SELECT a.attname, format_type(a.atttypid, a.atttypmod) AS attr_type, opc.opcname
    FROM pg_index i
    JOIN pg_attribute a ON a.attrelid = i.indexrelid
    JOIN pg_opclass opc ON opc.oid = i.indclass[a.attnum - 1]
    WHERE i.indexrelid = 'legacy_alter_type_idx'::regclass
    ORDER BY a.attnum;
-- the rebuilt index is consistent with the base table and serves reads.
SELECT yb_index_check('legacy_alter_type_idx'::regclass);
/*+ IndexScan(legacy_alter_type_test legacy_alter_type_idx) */ EXPLAIN (COSTS OFF)
SELECT id, id2 FROM legacy_alter_type_test
    WHERE id2 > 3 AND created_at > '2024-01-01 00:00:00+00' ORDER BY id;
/*+ IndexScan(legacy_alter_type_test legacy_alter_type_idx) */
SELECT id, id2 FROM legacy_alter_type_test
    WHERE id2 > 3 AND created_at > '2024-01-01 00:00:00+00' ORDER BY id;
-- an index backing a UNIQUE constraint is fixed up through the same path.
CREATE TABLE legacy_alter_type_unique_test (created_at timestamp UNIQUE);
INSERT INTO legacy_alter_type_unique_test VALUES ('2024-01-01 00:00:00');
ALTER TABLE legacy_alter_type_unique_test ALTER COLUMN created_at TYPE timestamptz;
SELECT format_type(a.atttypid, a.atttypmod) AS attr_type, opc.opcname
    FROM pg_index i
    JOIN pg_attribute a ON a.attrelid = i.indexrelid
    JOIN pg_opclass opc ON opc.oid = i.indclass[a.attnum - 1]
    WHERE i.indexrelid = 'legacy_alter_type_unique_test_created_at_key'::regclass;
SELECT yb_index_check('legacy_alter_type_unique_test_created_at_key'::regclass);
INSERT INTO legacy_alter_type_unique_test VALUES ('2024-01-01 00:00:00+00'); -- should fail.
-- a compatible no-rewrite change under the legacy path reuses the index in
-- place and updates its attribute metadata.
CREATE TABLE legacy_alter_compat_test (a varchar(10));
CREATE INDEX legacy_alter_compat_idx ON legacy_alter_compat_test (a);
CREATE TEMP TABLE legacy_alter_compat_info AS
    SELECT c.oid, c.relfilenode, a.atttypmod FROM pg_class c, pg_attribute a
    WHERE c.relname = 'legacy_alter_compat_idx' AND a.attrelid = c.oid AND a.attnum = 1;
ALTER TABLE legacy_alter_compat_test ALTER COLUMN a TYPE varchar(100);
SELECT CASE WHEN old.oid = new.oid THEN 'OID reused' ELSE 'OID changed' END,
        CASE WHEN old.relfilenode = new.relfilenode
            THEN 'relfilenode reused' ELSE 'relfilenode changed' END,
        CASE WHEN old.atttypmod <> a.atttypmod
            THEN 'atttypmod updated' ELSE 'atttypmod unchanged' END
    FROM legacy_alter_compat_info old, pg_class new, pg_attribute a
    WHERE new.relname = 'legacy_alter_compat_idx'
        AND a.attrelid = new.oid AND a.attnum = 1;
RESET yb_enable_alter_table_rewrite;
RESET timezone;
-- the restore-side failure that stale operator classes previously caused: an
-- explicit operator class that does not accept the column type is rejected.
CREATE TABLE restore_shape_test (id int, created_at timestamptz);
CREATE INDEX ON restore_shape_test (created_at timestamp_ops DESC); -- should fail.
