--
-- JOIN
-- Test JOIN clauses
--

CREATE TABLE J1_TBL (
  i integer,
  j integer,
  t text
);

CREATE TABLE J2_TBL (
  i integer,
  k integer
);


INSERT INTO J1_TBL VALUES (1, 4, 'one');
INSERT INTO J1_TBL VALUES (2, 3, 'two');
INSERT INTO J1_TBL VALUES (3, 2, 'three');
INSERT INTO J1_TBL VALUES (4, 1, 'four');
INSERT INTO J1_TBL VALUES (5, 0, 'five');
INSERT INTO J1_TBL VALUES (6, 6, 'six');
INSERT INTO J1_TBL VALUES (7, 7, 'seven');
INSERT INTO J1_TBL VALUES (8, 8, 'eight');
INSERT INTO J1_TBL VALUES (0, NULL, 'zero');
INSERT INTO J1_TBL VALUES (NULL, NULL, 'null');
INSERT INTO J1_TBL VALUES (NULL, 0, 'zero');

INSERT INTO J2_TBL VALUES (1, -1);
INSERT INTO J2_TBL VALUES (2, 2);
INSERT INTO J2_TBL VALUES (3, -3);
INSERT INTO J2_TBL VALUES (2, 4);
INSERT INTO J2_TBL VALUES (5, -5);
INSERT INTO J2_TBL VALUES (5, -5);
INSERT INTO J2_TBL VALUES (0, NULL);
INSERT INTO J2_TBL VALUES (NULL, NULL);
INSERT INTO J2_TBL VALUES (NULL, 0);

--
-- CORRELATION NAMES
-- Make sure that table/column aliases are supported
-- before diving into more complex join syntax.
--

SELECT '' AS "xxx", *
  FROM J1_TBL AS tx order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL tx  order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL AS t1 (a, b, c)  order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c)  order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c), J2_TBL t2 (d, e)  order by 1, 2, 3, 4, 5, 6;

SELECT '' AS "xxx", t1.a, t2.e
  FROM J1_TBL t1 (a, b, c), J2_TBL t2 (d, e)
  WHERE t1.a = t2.d  order by 1, 2, 3;


--
-- CROSS JOIN
-- Qualifications are not allowed on cross joins,
-- which degenerate into a standard unqualified inner join.
--

SELECT '' AS "xxx", *
  FROM J1_TBL CROSS JOIN J2_TBL  order by 1, 2, 3, 4, 5, 6;

-- ambiguous column
SELECT '' AS "xxx", i, k, t
  FROM J1_TBL CROSS JOIN J2_TBL;

-- resolve previous ambiguity by specifying the table name
SELECT '' AS "xxx", t1.i, k, t
  FROM J1_TBL t1 CROSS JOIN J2_TBL t2 order by 1, 2, 3, 4;

SELECT '' AS "xxx", ii, tt, kk
  FROM (J1_TBL CROSS JOIN J2_TBL)
    AS tx (ii, jj, tt, ii2, kk) order by 1, 2, 3, 4;

SELECT '' AS "xxx", tx.ii, tx.jj, tx.kk
  FROM (J1_TBL t1 (a, b, c) CROSS JOIN J2_TBL t2 (d, e))
    AS tx (ii, jj, tt, ii2, kk) order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL CROSS JOIN J2_TBL a CROSS JOIN J2_TBL b order by 1, 2, 3, 4, 5, 6, 7, 8;


--
--
-- Inner joins (equi-joins)
--
--

--
-- Inner joins (equi-joins) with USING clause
-- The USING syntax changes the shape of the resulting table
-- by including a column in the USING clause only once in the result.
--

-- Inner equi-join on specified column
SELECT '' AS "xxx", *
  FROM J1_TBL INNER JOIN J2_TBL USING (i) order by 1, 2, 3, 4, 5;

-- Same as above, slightly different syntax
SELECT '' AS "xxx", *
  FROM J1_TBL JOIN J2_TBL USING (i) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c) JOIN J2_TBL t2 (a, d) USING (a)
  ORDER BY a, d;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c) JOIN J2_TBL t2 (a, b) USING (b)
  ORDER BY b, t1.a;


--
-- NATURAL JOIN
-- Inner equi-join on all columns with the same name
--

SELECT '' AS "xxx", *
  FROM J1_TBL NATURAL JOIN J2_TBL order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c) NATURAL JOIN J2_TBL t2 (a, d) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c) NATURAL JOIN J2_TBL t2 (d, a) order by 1, 2, 3, 4, 5;

-- mismatch number of columns
-- currently, Postgres will fill in with underlying names
SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b) NATURAL JOIN J2_TBL t2 (a) order by 1, 2, 3, 4, 5;


--
-- Inner joins (equi-joins)
--

SELECT '' AS "xxx", *
  FROM J1_TBL JOIN J2_TBL ON (J1_TBL.i = J2_TBL.i) order by 1, 2, 3, 4, 5, 6;

SELECT '' AS "xxx", *
  FROM J1_TBL JOIN J2_TBL ON (J1_TBL.i = J2_TBL.k) order by 1, 2, 3, 4, 5, 6;


--
-- Non-equi-joins
--

SELECT '' AS "xxx", *
  FROM J1_TBL JOIN J2_TBL ON (J1_TBL.i <= J2_TBL.k) order by 1, 2, 3, 4, 5, 6;


--
-- Outer joins
-- Note that OUTER is a noise word
--

SELECT '' AS "xxx", *
  FROM J1_TBL LEFT OUTER JOIN J2_TBL USING (i)
  ORDER BY i, k, t;

SELECT '' AS "xxx", *
  FROM J1_TBL LEFT JOIN J2_TBL USING (i)
  ORDER BY i, k, t;

SELECT '' AS "xxx", *
  FROM J1_TBL RIGHT OUTER JOIN J2_TBL USING (i) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL RIGHT JOIN J2_TBL USING (i) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL FULL OUTER JOIN J2_TBL USING (i)
  ORDER BY i, k, t;

SELECT '' AS "xxx", *
  FROM J1_TBL FULL JOIN J2_TBL USING (i)
  ORDER BY i, k, t;

SELECT '' AS "xxx", *
  FROM J1_TBL LEFT JOIN J2_TBL USING (i) WHERE (k = 1) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL LEFT JOIN J2_TBL USING (i) WHERE (i = 1) order by 1, 2, 3, 4, 5;

--
-- semijoin selectivity for <>
--
-- explain (costs off)
-- select * from int4_tbl i4, tenk1 a
-- where exists(select * from tenk1 b
--              where a.twothousand = b.twothousand and a.fivethous <> b.fivethous)
--       and i4.f1 = a.tenthous;


--
-- More complicated constructs
--

--
-- Multiway full join
--

CREATE TABLE t1 (name TEXT, n INTEGER);
CREATE TABLE t2 (name TEXT, n INTEGER);
CREATE TABLE t3 (name TEXT, n INTEGER);

INSERT INTO t1 VALUES ( 'bb', 11 );
INSERT INTO t2 VALUES ( 'bb', 12 );
INSERT INTO t2 VALUES ( 'cc', 22 );
INSERT INTO t2 VALUES ( 'ee', 42 );
INSERT INTO t3 VALUES ( 'bb', 13 );
INSERT INTO t3 VALUES ( 'cc', 23 );
INSERT INTO t3 VALUES ( 'dd', 33 );

SELECT * FROM t1 FULL JOIN t2 USING (name) FULL JOIN t3 USING (name) order by 1, 2, 3, 4;

--
-- Test interactions of join syntax and subqueries
--

-- Basic cases (we expect planner to pull up the subquery here)
SELECT * FROM
(SELECT * FROM t2) as s2
INNER JOIN
(SELECT * FROM t3) s3
USING (name) order by 1, 2, 3;

SELECT * FROM
(SELECT * FROM t2) as s2
LEFT JOIN
(SELECT * FROM t3) s3
USING (name) order by 1, 2, 3;

SELECT * FROM
(SELECT * FROM t2) as s2
FULL JOIN
(SELECT * FROM t3) s3
USING (name) order by 1, 2, 3;

-- Cases with non-nullable expressions in subquery results;
-- make sure these go to null as expected
SELECT * FROM
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL INNER JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4, 5;

SELECT * FROM
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL LEFT JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4, 5;

SELECT * FROM
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL FULL JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4;

SELECT * FROM
(SELECT name, n as s1_n, 1 as s1_1 FROM t1) as s1
NATURAL INNER JOIN
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL INNER JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4, 5, 6, 7;

SELECT * FROM
(SELECT name, n as s1_n, 1 as s1_1 FROM t1) as s1
NATURAL FULL JOIN
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL FULL JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4, 5, 6, 7;

SELECT * FROM
(SELECT name, n as s1_n FROM t1) as s1
NATURAL FULL JOIN
  (SELECT * FROM
    (SELECT name, n as s2_n FROM t2) as s2
    NATURAL FULL JOIN
    (SELECT name, n as s3_n FROM t3) as s3
  ) ss2 order by 1, 2, 3, 4;

SELECT * FROM
(SELECT name, n as s1_n FROM t1) as s1
NATURAL FULL JOIN
  (SELECT * FROM
    (SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
    NATURAL FULL JOIN
    (SELECT name, n as s3_n FROM t3) as s3
  ) ss2 order by 1, 2, 3, 4, 5;


-- Test for propagation of nullability constraints into sub-joins

create temp table x (x1 int, x2 int);
insert into x values (1,11);
insert into x values (2,22);
insert into x values (3,null);
insert into x values (4,44);
insert into x values (5,null);

create temp table y (y1 int, y2 int);
insert into y values (1,111);
insert into y values (2,222);
insert into y values (3,333);
insert into y values (4,null);

select * from x order by 1, 2;
select * from y order by 1, 2;

select * from x left join y on (x1 = y1 and x2 is not null) order by 1, 2, 3, 4;
select * from x left join y on (x1 = y1 and y2 is not null) order by 1, 2, 3, 4;

select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1) order by 1, 2, 3, 4, 5, 6;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1 and x2 is not null) order by 1, 2, 3, 4, 5, 6;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1 and y2 is not null) order by 1, 2, 3, 4, 5, 6;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1 and xx2 is not null) order by 1, 2, 3, 4, 5, 6;
-- these should NOT give the same answers as above
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1) where (x2 is not null) order by 1;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1) where (y2 is not null) order by 1;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1) where (xx2 is not null) order by 1;

--
-- regression test: check for bug with propagation of implied equality
-- to outside an IN
--
select count(*) from tenk1 a where unique1 in
  (select unique1 from tenk1 b join tenk1 c using (unique1)
   where b.unique2 = 42);

--
-- regression test: check for failure to generate a plan with multiple
-- degenerate IN clauses
--
select count(*) from tenk1 x where
  x.unique1 in (select a.f1 from int4_tbl a,float8_tbl b where a.f1=b.f1) and
  x.unique1 = 0 and
  x.unique1 in (select aa.f1 from int4_tbl aa,float8_tbl bb where aa.f1=bb.f1);

-- try that with GEQO too
begin;
set geqo = on;
set geqo_threshold = 2;
select count(*) from tenk1 x where
  x.unique1 in (select a.f1 from int4_tbl a,float8_tbl b where a.f1=b.f1) and
  x.unique1 = 0 and
  x.unique1 in (select aa.f1 from int4_tbl aa,float8_tbl bb where aa.f1=bb.f1);
rollback;

-- yb_pg_join ends here. See: https://github.com/yugabyte/yugabyte-db/pull/5190#issuecomment-672361626
