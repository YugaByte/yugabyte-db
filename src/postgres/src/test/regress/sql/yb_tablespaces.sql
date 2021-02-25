-- Create Tablespace API tests
-- Create Tablespace should not work if neither LOCATION nor options
-- are specified
CREATE TABLESPACE x;
CREATE TABLESPACE x OWNER yugabyte;

-- Ill formed JSON
CREATE TABLESPACE x WITH (replica_placement='[{"cloud"}]');

-- num_replicas field missing.
CREATE TABLESPACE x WITH (replica_placement='{"placement_blocks":[{"cloud":"cloud1","region":"r1","zone":"z1","min_num_replicas":3}]}');

-- placement_blocks field missing.
CREATE TABLESPACE x WITH (replica_placement='{"num_replicas":3}');

-- Invalid value for num_replicas.
CREATE TABLESPACE x WITH (replica_placement='{"num_replicas":"three", "placement_blocks":[{"cloud":"cloud1","region":"r1","zone":"z1","min_num_replicas":3}]}');

-- Invalid value for min_num_replicas.
CREATE TABLESPACE x WITH (replica_placement='{"num_replicas":3, "placement_blocks":[{"cloud":"cloud1","region":"r1","zone":"z1","min_num_replicas":"three"}]}');

-- Invalid keys in the placement policy.
CREATE TABLESPACE x WITH (replica_placement='{"num_replicas":3, "placement_blocks":[{"cloud":"cloud1","region":"r1","zone":"z1","min_num_replicas":3,"invalid_key":"invalid_value"}]}');

-- Sum of min_num_replicas greater than num_replicas.
CREATE TABLESPACE y WITH (replica_placement='{"num_replicas":3, "placement_blocks":[{"cloud":"cloud1","region":"r1","zone":"z1","min_num_replicas":2},{"cloud":"cloud2","region":"r2", "zone":"z2", "min_num_replicas":2}]}');

-- Positive cases
-- Tablespace without replica_placement option.
CREATE TABLESPACE x LOCATION '/data';

-- Sum of min_num_replicas lesser than num_replicas.
CREATE TABLESPACE y WITH (replica_placement='{"num_replicas":3, "placement_blocks":[{"cloud":"cloud1","region":"r1","zone":"z1","min_num_replicas":1},{"cloud":"cloud2","region":"r2", "zone":"z2", "min_num_replicas":1}]}');

-- Sum of min_num_replicas equal to num_replicas.
CREATE TABLESPACE z WITH (replica_placement='{"num_replicas":3, "placement_blocks":[{"cloud":"cloud1","region":"r1","zone":"z1","min_num_replicas":1},{"cloud":"cloud2","region":"r2", "zone":"z2", "min_num_replicas":2}]}');

-- describe command
\db

DROP TABLESPACE x;
DROP TABLESPACE y;

-- create a tablespace using WITH clause
CREATE TABLESPACE regress_tblspacewith WITH (some_nonexistent_parameter = true); -- fail
CREATE TABLESPACE regress_tblspacewith WITH (replica_placement='{"num_replicas":3, "placement_blocks":[{"cloud":"cloud1","region":"r1","zone":"z1","min_num_replicas":1},{"cloud":"cloud2","region":"r2", "zone":"z2", "min_num_replicas":2}]}'); -- ok

-- check to see if WITH clause parameter was used
SELECT spcoptions FROM pg_tablespace WHERE spcname = 'regress_tblspacewith';

DROP TABLESPACE regress_tblspacewith;

-- create a tablespace we can use
CREATE TABLESPACE regress_tblspace LOCATION '/data';

-- try setting and resetting some properties for the new tablespace
ALTER TABLESPACE regress_tblspace SET (random_page_cost = 1.0, seq_page_cost = 1.1);

-- Enable these tests after ALTER is supported.
/*
ALTER TABLESPACE regress_tblspace SET (some_nonexistent_parameter = true);  -- fail
ALTER TABLESPACE regress_tblspace RESET (random_page_cost = 2.0); -- fail
ALTER TABLESPACE regress_tblspace RESET (random_page_cost, effective_io_concurrency); -- ok
*/

-- create a schema we can use
CREATE SCHEMA testschema;

-- try a table
CREATE TABLE testschema.foo (i int) TABLESPACE regress_tblspace;
SELECT relname, spcname FROM pg_catalog.pg_tablespace t, pg_catalog.pg_class c
    where c.reltablespace = t.oid AND c.relname = 'foo';

INSERT INTO testschema.foo VALUES(1);
INSERT INTO testschema.foo VALUES(2);

-- tables from dynamic sources
CREATE TABLE testschema.asselect TABLESPACE regress_tblspace AS SELECT 1;
SELECT relname, spcname FROM pg_catalog.pg_tablespace t, pg_catalog.pg_class c
    where c.reltablespace = t.oid AND c.relname = 'asselect';

PREPARE selectsource(int) AS SELECT $1;
CREATE TABLE testschema.asexecute TABLESPACE regress_tblspace
    AS EXECUTE selectsource(2);
/*
SELECT relname, spcname FROM pg_catalog.pg_tablespace t, pg_catalog.pg_class c
    where c.reltablespace = t.oid AND c.relname = 'asexecute';
*/

-- index
CREATE INDEX foo_idx on testschema.foo(i) TABLESPACE regress_tblspace;
SELECT relname, spcname FROM pg_catalog.pg_tablespace t, pg_catalog.pg_class c
    where c.reltablespace = t.oid AND c.relname = 'foo_idx';

-- partitioned index
CREATE TABLE testschema.part (a int) PARTITION BY LIST (a);
CREATE TABLE testschema.part1 PARTITION OF testschema.part FOR VALUES IN (1);
CREATE INDEX part_a_idx ON testschema.part (a) TABLESPACE regress_tblspace;
CREATE TABLE testschema.part2 PARTITION OF testschema.part FOR VALUES IN (2);
SELECT relname, spcname FROM pg_catalog.pg_tablespace t, pg_catalog.pg_class c
    where c.reltablespace = t.oid AND c.relname LIKE 'part%_idx' ORDER BY relname;

-- check that default_tablespace doesn't affect ALTER TABLE index rebuilds
CREATE TABLE testschema.test_default_tab(id bigint) TABLESPACE regress_tblspace;
INSERT INTO testschema.test_default_tab VALUES (1);
CREATE INDEX test_index1 on testschema.test_default_tab (id);
CREATE INDEX test_index2 on testschema.test_default_tab (id) TABLESPACE regress_tblspace;
\d testschema.test_index1
\d testschema.test_index2
-- use a custom tablespace for default_tablespace
SET default_tablespace TO regress_tblspace;
-- tablespace should not change if no rewrite
ALTER TABLE testschema.test_default_tab ALTER id TYPE bigint;
\d testschema.test_index1
\d testschema.test_index2
SELECT * FROM testschema.test_default_tab;
-- tablespace should not change even if there is an index rewrite
ALTER TABLE testschema.test_default_tab ALTER id TYPE int;
\d testschema.test_index1
\d testschema.test_index2
SELECT * FROM testschema.test_default_tab;
-- now use the default tablespace for default_tablespace
SET default_tablespace TO '';
-- tablespace should not change if no rewrite
ALTER TABLE testschema.test_default_tab ALTER id TYPE int;
\d testschema.test_index1
\d testschema.test_index2
-- tablespace should not change even if there is an index rewrite
ALTER TABLE testschema.test_default_tab ALTER id TYPE bigint;
\d testschema.test_index1
\d testschema.test_index2
DROP TABLE testschema.test_default_tab;
-- check that default_tablespace affects index additions in ALTER TABLE
CREATE TABLE testschema.test_tab(id int) TABLESPACE regress_tblspace;
INSERT INTO testschema.test_tab VALUES (1);
SET default_tablespace TO regress_tblspace;
ALTER TABLE testschema.test_tab ADD CONSTRAINT test_tab_unique UNIQUE (id);
SET default_tablespace TO '';
ALTER TABLE testschema.test_tab ADD CONSTRAINT test_tab_pkey PRIMARY KEY (id);
\d testschema.test_tab_unique
/*
\d testschema.test_tab_pkey
*/
SELECT * FROM testschema.test_tab;
DROP TABLE testschema.test_tab;

-- let's try moving a table from one place to another
CREATE TABLE testschema.atable AS VALUES (1), (2);
CREATE UNIQUE INDEX anindex ON testschema.atable(column1);
ALTER TABLE testschema.atable SET TABLESPACE regress_tblspace;
/*
ALTER INDEX testschema.anindex SET TABLESPACE regress_tblspace;
ALTER INDEX testschema.part_a_idx SET TABLESPACE pg_global;
ALTER INDEX testschema.part_a_idx SET TABLESPACE pg_default;
ALTER INDEX testschema.part_a_idx SET TABLESPACE regress_tblspace;
*/
INSERT INTO testschema.atable VALUES(3);
INSERT INTO testschema.atable VALUES(1);
SELECT COUNT(*) FROM testschema.atable;

-- No such tablespace
CREATE TABLE bar (i int) TABLESPACE regress_nosuchspace;

-- Fail, not empty
\set VERBOSITY terse \\ -- suppress dependency details.
DROP TABLESPACE regress_tblspace;
\set VERBOSITY default

CREATE ROLE regress_tablespace_user1 login;
CREATE ROLE regress_tablespace_user2 login;
GRANT USAGE ON SCHEMA testschema TO regress_tablespace_user2;

ALTER TABLESPACE regress_tblspace OWNER TO regress_tablespace_user1;

CREATE TABLE testschema.tablespace_acl (c int);
-- new owner lacks permission to create this index from scratch
CREATE INDEX k ON testschema.tablespace_acl (c) TABLESPACE regress_tblspace;
ALTER TABLE testschema.tablespace_acl OWNER TO regress_tablespace_user2;

SET SESSION ROLE regress_tablespace_user2;
CREATE TABLE tablespace_table (i int) TABLESPACE regress_tblspace; -- fail
ALTER TABLE testschema.tablespace_acl ALTER c TYPE bigint;
RESET ROLE;

ALTER TABLESPACE regress_tblspace RENAME TO regress_tblspace_renamed;
/*
ALTER TABLE ALL IN TABLESPACE regress_tblspace_renamed SET TABLESPACE pg_default;
ALTER INDEX ALL IN TABLESPACE regress_tblspace_renamed SET TABLESPACE pg_default;

-- Should show notice that nothing was done
ALTER TABLE ALL IN TABLESPACE regress_tblspace_renamed SET TABLESPACE pg_default;

-- Should succeed
DROP TABLESPACE regress_tblspace_renamed;
*/
\set VERBOSITY terse \\ -- suppress cascade details
DROP SCHEMA testschema CASCADE;
\set VERBOSITY default
DROP ROLE regress_tablespace_user1;
DROP TABLESPACE regress_tblspace;
DROP ROLE regress_tablespace_user1;
DROP ROLE regress_tablespace_user2;
