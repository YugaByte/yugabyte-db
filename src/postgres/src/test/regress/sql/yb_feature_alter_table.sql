--
-- ALTER_TABLE
-- add, rename, drop, alter type of attribute
--

CREATE TABLE tmp (initial int4);

ALTER TABLE tmp1 ADD COLUMN xmin integer; -- fails

ALTER TABLE tmp ADD COLUMN b name;

ALTER TABLE tmp ADD COLUMN c text;

ALTER TABLE tmp ADD COLUMN d float8;

ALTER TABLE tmp ADD COLUMN e float4;

ALTER TABLE tmp ADD COLUMN f int2;

ALTER TABLE tmp ADD COLUMN i char;

ALTER TABLE tmp ADD COLUMN k int4;

ALTER TABLE tmp ADD COLUMN m xid;

ALTER TABLE tmp ADD COLUMN n oidvector;

ALTER TABLE tmp ADD COLUMN v timestamp;

ALTER TABLE tmp ADD COLUMN y float4[];

ALTER TABLE tmp ADD COLUMN z int2[];

INSERT INTO tmp (b, c, d, e, f, i, k, m, n, v, y, z)
   VALUES ('name', 'text', 4.1, 4.1, 2, 'c', 314159, '512',
   '1 2 3 4 5 6 7 8', 'epoch', '{1.0,2.0,3.0,4.0}', '{1,2,3,4}');

SELECT * FROM tmp;

DROP TABLE tmp;

-- alter table / drop column tests
-- try altering system catalogs, should fail
alter table pg_class drop column relname;

-- try altering non-existent table, should fail
alter table nosuchtable drop column bar;

-- test dropping columns
create table atacc1 (a int4 not null, b int4, c int4 not null, d int4);
insert into atacc1 values (1, 2, 3, 4);
alter table atacc1 drop a;
alter table atacc1 drop a;

-- SELECTs
select * from atacc1;
select * from atacc1 order by a;
select * from atacc1 order by "........pg.dropped.1........";
select * from atacc1 group by a;
select * from atacc1 group by "........pg.dropped.1........";
select atacc1.* from atacc1;
select a from atacc1;
select atacc1.a from atacc1;
select b,c,d from atacc1;
select a,b,c,d from atacc1;
select * from atacc1 where a = 1;
select "........pg.dropped.1........" from atacc1;
select atacc1."........pg.dropped.1........" from atacc1;
select "........pg.dropped.1........",b,c,d from atacc1;
select * from atacc1 where "........pg.dropped.1........" = 1;

-- UPDATEs
update atacc1 set a = 3;
update atacc1 set b = 2 where a = 3;
update atacc1 set "........pg.dropped.1........" = 3;
update atacc1 set b = 2 where "........pg.dropped.1........" = 3;

-- INSERTs
insert into atacc1 values (10, 11, 12, 13);
insert into atacc1 values (default, 11, 12, 13);
insert into atacc1 values (11, 12, 13);
insert into atacc1 (a) values (10);
insert into atacc1 (a) values (default);
insert into atacc1 (a,b,c,d) values (10,11,12,13);
insert into atacc1 (a,b,c,d) values (default,11,12,13);
insert into atacc1 (b,c,d) values (11,12,13);
insert into atacc1 ("........pg.dropped.1........") values (10);
insert into atacc1 ("........pg.dropped.1........") values (default);
insert into atacc1 ("........pg.dropped.1........",b,c,d) values (10,11,12,13);
insert into atacc1 ("........pg.dropped.1........",b,c,d) values (default,11,12,13);

-- DELETEs
delete from atacc1 where a = 3;
delete from atacc1 where "........pg.dropped.1........" = 3;
delete from atacc1;

-- try dropping a non-existent column, should fail
alter table atacc1 drop bar;

-- try dropping the oid column, should succeed
alter table atacc1 drop oid;

-- try dropping the xmin column, should fail
alter table atacc1 drop xmin;

-- try creating a view and altering that, should fail
create view myview as select * from atacc1;
select * from myview;
alter table myview drop d;
drop view myview;

-- test some commands to make sure they fail on the dropped column
create index "testing_idx" on atacc1(a);
create index "testing_idx" on atacc1("........pg.dropped.1........");
alter table atacc1 rename a to z;
alter table atacc1 add constraint checka check (a >= 0);
alter table if exists atacc1 add constraint checka check (a >= 0);

-- test create as and select into
insert into atacc1 values (21, 22, 23);
create table test1 as select * from atacc1;
select * from test1;
drop table test1;
select * into test2 from atacc1;
select * from test2;
drop table test2;

-- test constraints
alter table atacc1 add constraint checkb check (b < 0); -- should fail
alter table if exists atacc1 add constraint checkb check (b < 0); -- should fail
alter table atacc1 add constraint checkb check (b > 0);
alter table atacc1 add constraint checkb2 check (b > 10);
alter table atacc1 add constraint checkb3 check (b > 10);
insert into atacc1 values (5, 5, 5); -- should fail
alter table atacc1 drop constraint checkb2;
alter table if exists atacc1 add constraint checkb2 check (b > 10);
insert into atacc1 values (5, 5, 5); -- should fail
alter table atacc1 drop constraint checkb2;
insert into atacc1 values (5, 5, 5);
alter table atacc1 drop constraint checkb;
alter table atacc1 drop constraint if exists checkb2;
alter table atacc1 drop constraint if exists checkb3;
delete from atacc1 where b = 5;

-- test rename
alter table atacc1 rename b to e;
select * from atacc1;

-- try dropping all columns
alter table atacc1 drop c;
alter table atacc1 drop d;
alter table atacc1 drop b;
select * from atacc1;

drop table atacc1;

-- test dropping primary key constraints
CREATE TABLE with_simple_pk_i (i int PRIMARY KEY);
ALTER TABLE with_simple_pk_i DROP CONSTRAINT with_simple_pk_i_pkey;
INSERT INTO with_simple_pk_i VALUES (1);
INSERT INTO with_simple_pk_i VALUES (1);
DROP TABLE with_simple_pk_i;
--
CREATE TABLE with_simple_pk_ij (i int, j int, PRIMARY KEY(i, j));
ALTER TABLE with_simple_pk_ij DROP CONSTRAINT with_simple_pk_ij_pkey;
INSERT INTO with_simple_pk_ij VALUES (1, 1);
INSERT INTO with_simple_pk_ij VALUES (1, 1);
DROP TABLE with_simple_pk_ij;
--
CREATE TABLE with_named_pk_i (i int CONSTRAINT named_pk_i PRIMARY KEY);
ALTER TABLE with_named_pk_i DROP CONSTRAINT named_pk_i;
INSERT INTO with_named_pk_i VALUES (1);
INSERT INTO with_named_pk_i VALUES (1);
DROP TABLE with_named_pk_i;
--
CREATE TABLE with_named_pk_ij (i int, j int, CONSTRAINT named_pk_ij PRIMARY KEY (i HASH, j ASC));
ALTER TABLE with_named_pk_ij DROP CONSTRAINT named_pk_ij;
INSERT INTO with_named_pk_ij VALUES (1, 1);
INSERT INTO with_named_pk_ij VALUES (1, 1);
DROP TABLE with_named_pk_ij;

--
-- rename
--
CREATE TABLE tmp (regtable int);

ALTER TABLE tmp RENAME TO tmp_new;

SELECT * FROM tmp_new;
SELECT * FROM tmp;		-- should fail

DROP TABLE tmp_new;

-- alter table / alter column [set/drop] not null tests
-- try altering system catalogs, should fail
alter table pg_class alter column relname drop not null;
alter table pg_class alter relname set not null;

-- try altering non-existent table, should fail
alter table non_existent alter column bar set not null;
alter table non_existent alter column bar drop not null;

-- try altering non-existent table with IF EXISTS clause, should pass with a notice message
alter table if exists non_existent alter column bar set not null;
alter table if exists non_existent alter column bar drop not null;
alter table if exists non_existent
    add constraint some_constraint_name foreign key (k) references atacc1;

-- test setting columns to null and not null and vice versa
-- test checking for null values and primary key
create table atacc1tmp (test int not null PRIMARY KEY);
alter table atacc1tmp alter column test drop not null;
drop table atacc1tmp;

create table atacc1 (test int not null);
alter table atacc1 alter column test drop not null;
insert into atacc1 values (null);
alter table atacc1 alter test set not null;
delete from atacc1;
alter table atacc1 alter test set not null;

-- try altering a non-existent column, should fail
alter table atacc1 alter bar set not null;
alter table atacc1 alter bar drop not null;

-- -- try altering the oid column, should fail
-- alter table atacc1 alter oid set not null;
-- alter table atacc1 alter oid drop not null;

-- try creating a view and altering that, should fail
create view myview as select * from atacc1;
alter table myview alter column test drop not null;
alter table myview alter column test set not null;
drop view myview;

drop table atacc1;

-- test setting and removing default values
create table def_test (
	c1	int4 default 5,
	c2	text default 'initial_default'
);
insert into def_test default values;
alter table def_test alter column c1 drop default;
insert into def_test default values;
alter table def_test alter column c2 drop default;
insert into def_test default values;
alter table def_test alter column c1 set default 10;
alter table def_test alter column c2 set default 'new_default';
insert into def_test default values;
select * from def_test order by c1, c2;

-- set defaults to an incorrect type: this should fail
alter table def_test alter column c1 set default 'wrong_datatype';
alter table def_test alter column c2 set default 20;

-- set defaults on a non-existent column: this should fail
alter table def_test alter column c3 set default 30;

-- set defaults on views: we need to create a view, add a rule
-- to allow insertions into it, and then alter the view to add
-- a default
create view def_view_test as select * from def_test;
create rule def_view_test_ins as
	on insert to def_view_test
	do instead insert into def_test select new.*;
insert into def_view_test default values;
alter table def_view_test alter column c1 set default 45;
insert into def_view_test default values;
alter table def_view_test alter column c2 set default 'view_default';
insert into def_view_test default values;
select * from def_view_test ORDER BY c1 NULLS FIRST, c2 NULLS FIRST;

drop rule def_view_test_ins on def_view_test;
drop view def_view_test;
drop table def_test;

-- test CREATE or REPLACE VIEW
create or replace view v as select 17 as c1;
select * from v;
create or replace view v as select 42 as c1;
select * from v;
create or replace view v as select 11 as
	c1, 12 AS c2, 13 AS c3; -- testing replace/alter view by adding columns
select * from v;
drop view v;
-- test ADD COLUMN IF NOT EXISTS
CREATE TABLE test_add_column(c1 integer);
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN c2 integer;
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN c2 integer; -- fail because c2 already exists
ALTER TABLE ONLY test_add_column
	ADD COLUMN c2 integer; -- fail because c2 already exists
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN IF NOT EXISTS c2 integer; -- skipping because c2 already exists
ALTER TABLE ONLY test_add_column
	ADD COLUMN IF NOT EXISTS c2 integer; -- skipping because c2 already exists
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN c2 integer, -- fail because c2 already exists
	ADD COLUMN c3 integer;
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN IF NOT EXISTS c2 integer, -- skipping because c2 already exists
	ADD COLUMN c3 integer; -- fail because c3 already exists
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN IF NOT EXISTS c2 integer, -- skipping because c2 already exists
	ADD COLUMN IF NOT EXISTS c3 integer; -- skipping because c3 already exists
\d test_add_column
ALTER TABLE test_add_column
	ADD COLUMN IF NOT EXISTS c2 integer, -- skipping because c2 already exists
	ADD COLUMN IF NOT EXISTS c3 integer, -- skipping because c3 already exists
	ADD COLUMN c4 integer;
\d test_add_column
DROP TABLE test_add_column;

-- test Add/Set/Drop Identity
CREATE TABLE test_identity(
    c1 integer NOT NULL,
    c2 integer NOT NULL
);

-- test ADD GENERATED ALWAYS
ALTER TABLE test_identity ALTER COLUMN c1 ADD GENERATED ALWAYS AS IDENTITY;
\d test_identity;
INSERT INTO test_identity (c1, c2) VALUES (0, 0);
INSERT INTO test_identity (c2) VALUES (1);
INSERT INTO test_identity (c2) VALUES (2);
-- test DROP IDENTITY
ALTER TABLE test_identity ALTER COLUMN c1 DROP IDENTITY;
\d test_identity;
ALTER TABLE test_identity ALTER COLUMN c1 DROP IDENTITY;
\d test_identity;

-- test ADD GENERATED BY DEFAULT
ALTER TABLE test_identity ALTER COLUMN c1 ADD GENERATED BY DEFAULT AS IDENTITY;
\d test_identity;
INSERT INTO test_identity (c1, c2) VALUES (1000, 3);
INSERT INTO test_identity (c2) VALUES (4);
INSERT INTO test_identity (c2) VALUES (5);
-- test DROP IDENTITY IF EXISTS
ALTER TABLE test_identity ALTER COLUMN c1 DROP IDENTITY IF EXISTS;
\d test_identity;
ALTER TABLE test_identity ALTER COLUMN c1 DROP IDENTITY IF EXISTS;
\d test_identity;

-- test SET GENERATED
ALTER TABLE test_identity ALTER COLUMN c1 ADD GENERATED BY DEFAULT AS IDENTITY (INCREMENT 10 MINVALUE 10);
\d test_identity;
ALTER TABLE test_identity ALTER c1 SET GENERATED ALWAYS;
\d test_identity;
ALTER TABLE test_identity ALTER c1 SET GENERATED BY DEFAULT;
\d test_identity;
INSERT INTO test_identity (c2) VALUES (6);
INSERT INTO test_identity (c2) VALUES (7);
ALTER TABLE test_identity ALTER c1 SET INCREMENT 20;
INSERT INTO test_identity (c2) VALUES (8);
INSERT INTO test_identity (c2) VALUES (9);
ALTER TABLE test_identity ALTER c1 RESTART;
INSERT INTO test_identity (c2) VALUES (10);
INSERT INTO test_identity (c2) VALUES (11);
ALTER TABLE test_identity ALTER c1 SET INCREMENT 50 RESTART WITH 100;
INSERT INTO test_identity (c2) VALUES (12);
INSERT INTO test_identity (c2) VALUES (13);
SELECT * FROM test_identity ORDER BY c2;

DROP TABLE test_identity;
-- Test that updating table after dropping a column does not result in spurious error #1969
create table test_update_dropped(a int, b int);
insert into test_update_dropped(a, b) values(1, 1);
update test_update_dropped set a = 2 where a = 1;
alter table test_update_dropped drop column b;
update test_update_dropped set a = 3 where a = 2;
\d test_update_dropped;
select * from test_update_dropped;
DROP TABLE test_update_dropped;

-- Test that deleting table with a returning clause after dropping a column
-- does not result in spurious error #2938.
create table test_delete_dropped(a int, b int);
insert into test_delete_dropped(a, b) values(1, 1), (2, 2);
alter table test_delete_dropped drop column b;
delete from test_delete_dropped where a = 1 returning *;
\d test_delete_dropped;
select * from test_delete_dropped;
DROP TABLE test_delete_dropped;

-- Test ALTER TABLE _ ALTER COLUMN _ TYPE _
create table test_alter_column_type(a varchar(1));
alter table test_alter_column_type add column b name;
alter table test_alter_column_type add column c text;
alter table test_alter_column_type add column d char(1);
alter table test_alter_column_type add column e varbit(1);
alter table test_alter_column_type add column f int;
alter table test_alter_column_type add column g varchar;
alter table test_alter_column_type add column h varbit;
insert into test_alter_column_type values ('a', 'b', 'c', 'd', B'1', 1, 'g', B'1');
select * from test_alter_column_type;
\d test_alter_column_type

alter table test_alter_column_type alter column a type varchar(1);
alter table test_alter_column_type alter column a type varchar(5);
alter table test_alter_column_type alter column a type varchar(1); --fails
alter table test_alter_column_type alter column a type char(10); --fails

alter table test_alter_column_type alter column b type name;
alter table test_alter_column_type alter column b type varchar(100); --fails

alter table test_alter_column_type alter column c type text;
alter table test_alter_column_type alter column c type varchar(100); --fails

alter table test_alter_column_type alter column d type char(1);
alter table test_alter_column_type alter column d type varchar(100); --fails
alter table test_alter_column_type alter column d type char(100); --fails

alter table test_alter_column_type alter column e type varbit(1);
alter table test_alter_column_type alter column e type varbit(5);
alter table test_alter_column_type alter column e type varbit(1); --fails

alter table test_alter_column_type alter column f type varchar(100); --fails
alter table test_alter_column_type alter column f type varbit(100); --fails
alter table test_alter_column_type alter column f type int;

alter table test_alter_column_type alter column g type varchar;
alter table test_alter_column_type alter column g type varchar(5); --fails
alter table test_alter_column_type alter column g type text; --fails

alter table test_alter_column_type alter column h type varbit;
alter table test_alter_column_type alter column h type varbit(5); --fails

insert into test_alter_column_type values ('abcde', '-', '-', '-', B'10101', 0, '-', B'0');

select * from test_alter_column_type order by a;
\d test_alter_column_type

alter table test_alter_column_type alter column a type varchar;
alter table test_alter_column_type alter column e type varbit;

\d test_alter_column_type

DROP TABLE test_alter_column_type;


-- Test ALTER TABLE _ ALTER COLUMN _ TYPE _ with an index (Issue #6113)
create table test_alter_column_type_with_index(i int primary key, a varchar(50));

alter table test_alter_column_type_with_index alter column a type varchar(100);
insert into test_alter_column_type_with_index values (1, 'abcde'), (2, 'fghij');

create index on test_alter_column_type_with_index(a);

alter table test_alter_column_type_with_index alter column a type varchar(100);
alter table test_alter_column_type_with_index alter column a type varchar(200);

insert into test_alter_column_type_with_index values (3, 'klmno'), (4, 'pqrst');
select * from test_alter_column_type_with_index order by a;
\d test_alter_column_type_with_index

DROP TABLE test_alter_column_type_with_index;
