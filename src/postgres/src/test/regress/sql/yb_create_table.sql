--
-- YB_TABLE Testsuite: Testing DDL Statments for TABLE.
--

--
-- CREATE TABLE AS SELECT
--
CREATE TABLE table_create_org(
			 col_smallint			SMALLINT,
			 col_integer			INTEGER,
			 col_bigint				BIGINT,
			 col_real					REAL,
			 col_double				DOUBLE PRECISION,
			 col_char					CHARACTER(7),
			 col_varchar			VARCHAR(7),
			 col_text					TEXT,
			 col_bytea				BYTEA,
			 col_timestamp		TIMESTAMP(2),
			 col_timestamp_tz TIMESTAMP WITH TIME ZONE,
			 col_bool					BOOLEAN,
			 col_array_int		INTEGER[],
			 col_array_text		TEXT[],
			 PRIMARY KEY(col_smallint));
--
INSERT INTO table_create_org VALUES(
			 1,
			 1,
			 1,
			 1.1,
			 1.1,
			 'one',
			 'one',
			 'one',
			 E'\\x11F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'January 1, 2019 01:01:01.1111',
			 'January 1, 2019 01:01:01.1111 PST AD',
			 TRUE,
			 '{ 1, 1, 1 }',
			 '{ "one", "one", "one" }');
INSERT INTO table_create_org VALUES(
			 11,
			 1,
			 1,
			 1.1,
			 1.1,
			 'one',
			 'one',
			 'one',
			 E'\\x11F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'January 1, 2019 01:01:01.1111',
			 'January 1, 2019 01:01:01.1111 PST AD',
			 TRUE,
			 '{ 1, 1, 1 }',
			 '{ "one", "one", "one" }');
--
INSERT INTO table_create_org VALUES(
			 2,
			 2,
			 2,
			 2.2,
			 2.2,
			 'two',
			 'two',
			 'two',
			 E'\\x22F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'February 2, 2019 02:02:02.2222',
			 'February 2, 2019 02:02:02.2222 PST AD',
			 TRUE,
			 '{ 2, 2, 2 }',
			 '{ "two", "two", "two" }');
INSERT INTO table_create_org VALUES(
			 12,
			 2,
			 2,
			 2.2,
			 2.2,
			 'two',
			 'two',
			 'two',
			 E'\\x22F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'February 2, 2019 02:02:02.2222',
			 'February 2, 2019 02:02:02.2222 PST AD',
			 TRUE,
			 '{ 2, 2, 2 }',
			 '{ "two", "two", "two" }');
--
INSERT INTO table_create_org VALUES(
			 3,
			 3,
			 3,
			 3.3,
			 3.3,
			 'three',
			 'three',
			 'three',
			 E'\\x33F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'March 3, 2019 03:03:03.3333',
			 'March 3, 2019 03:03:03.3333 PST AD',
			 TRUE,
			 '{ 3, 3, 3 }',
			 '{ "three", "three", "three" }');
INSERT INTO table_create_org VALUES(
			 13,
			 3,
			 3,
			 3.3,
			 3.3,
			 'three',
			 'three',
			 'three',
			 E'\\x33F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'March 3, 2019 03:03:03.3333',
			 'March 3, 2019 03:03:03.3333 PST AD',
			 TRUE,
			 '{ 3, 3, 3 }',
			 '{ "three", "three", "three" }');
--
INSERT INTO table_create_org VALUES(
			 4,
			 4,
			 4,
			 4.4,
			 4.4,
			 'four',
			 'four',
			 'four',
			 E'\\x44F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'April 4, 2019 04:04:04.4444',
			 'April 4, 2019 04:04:04.4444 PST AD',
			 TRUE,
			 '{ 4, 4, 4 }',
			 '{ "four", "four", "four" }');
INSERT INTO table_create_org VALUES(
			 14,
			 4,
			 4,
			 4.4,
			 4.4,
			 'four',
			 'four',
			 'four',
			 E'\\x44F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'April 4, 2019 04:04:04.4444',
			 'April 4, 2019 04:04:04.4444 PST AD',
			 TRUE,
			 '{ 4, 4, 4 }',
			 '{ "four", "four", "four" }');
--
INSERT INTO table_create_org VALUES(
			 5,
			 5,
			 5,
			 5.5,
			 5.5,
			 'five',
			 'five',
			 'five',
			 E'\\x55F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'May 5, 2019 05:05:05.5555',
			 'May 5, 2019 05:05:05.5555 PST AD',
			 TRUE,
			 '{ 5, 5, 5 }',
			 '{ "five", "five", "five" }');
INSERT INTO table_create_org VALUES(
			 15,
			 5,
			 5,
			 5.5,
			 5.5,
			 'five',
			 'five',
			 'five',
			 E'\\x55F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'May 5, 2019 05:05:05.5555',
			 'May 5, 2019 05:05:05.5555 PST AD',
			 TRUE,
			 '{ 5, 5, 5 }',
			 '{ "five", "five", "five" }');
--
INSERT INTO table_create_org VALUES(
			 6,
			 6,
			 6,
			 6.6,
			 6.6,
			 'six',
			 'six',
			 'six',
			 E'\\x66F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'June 6, 2019 06:06:06.6666',
			 'June 6, 2019 06:06:06.6666 PST AD',
			 TRUE,
			 '{ 6, 6, 6 }',
			 '{ "six", "six", "six" }');
INSERT INTO table_create_org VALUES(
			 16,
			 6,
			 6,
			 6.6,
			 6.6,
			 'six',
			 'six',
			 'six',
			 E'\\x66F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'June 6, 2019 06:06:06.6666',
			 'June 6, 2019 06:06:06.6666 PST AD',
			 TRUE,
			 '{ 6, 6, 6 }',
			 '{ "six", "six", "six" }');
--
INSERT INTO table_create_org VALUES(
			 7,
			 7,
			 7,
			 7.7,
			 7.7,
			 'seven',
			 'seven',
			 'seven',
			 E'\\x77F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'July 7, 2019 07:07:07.7777',
			 'July 7, 2019 07:07:07.7777 PST AD',
			 TRUE,
			 '{ 7, 7, 7 }',
			 '{ "seven", "seven", "seven" }');
INSERT INTO table_create_org VALUES(
			 17,
			 7,
			 7,
			 7.7,
			 7.7,
			 'seven',
			 'seven',
			 'seven',
			 E'\\x77F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'July 7, 2019 07:07:07.7777',
			 'July 7, 2019 07:07:07.7777 PST AD',
			 TRUE,
			 '{ 7, 7, 7 }',
			 '{ "seven", "seven", "seven" }');
--
INSERT INTO table_create_org VALUES(
			 8,
			 8,
			 8,
			 8.8,
			 8.8,
			 'eight',
			 'eight',
			 'eight',
			 E'\\x88F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'August 8, 2019 08:08:08.8888',
			 'August 8, 2019 08:08:08.8888 PST AD',
			 TRUE,
			 '{ 8, 8, 8 }',
			 '{ "eight", "eight", "eight" }');
INSERT INTO table_create_org VALUES(
			 18,
			 8,
			 8,
			 8.8,
			 8.8,
			 'eight',
			 'eight',
			 'eight',
			 E'\\x88F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'August 8, 2019 08:08:08.8888',
			 'August 8, 2019 08:08:08.8888 PST AD',
			 TRUE,
			 '{ 8, 8, 8 }',
			 '{ "eight", "eight", "eight" }');
--
INSERT INTO table_create_org VALUES(
			 9,
			 9,
			 9,
			 9.9,
			 9.9,
			 'nine',
			 'nine',
			 'nine',
			 E'\\x99F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'September 9, 2019 09:09:09.9999',
			 'September 9, 2019 09:09:09.9999 PST AD',
			 TRUE,
			 '{ 9, 9, 9 }',
			 '{ "nine", "nine", "nine" }');
INSERT INTO table_create_org VALUES(
			 19,
			 9,
			 9,
			 9.9,
			 9.9,
			 'nine',
			 'nine',
			 'nine',
			 E'\\x99F1E2D3C4B5A6079889706A5B4C3D2E1F',
			 'September 9, 2019 09:09:09.9999',
			 'September 9, 2019 09:09:09.9999 PST AD',
			 TRUE,
			 '{ 9, 9, 9 }',
			 '{ "nine", "nine", "nine" }');
--
CREATE TABLE table_create_ctas_nodata AS SELECT * FROM table_create_org WITH NO DATA;
--
SELECT * FROM table_create_ctas_nodata;
--
CREATE TABLE table_create_ctas2_nodata
			 AS SELECT col_smallint id,
			 					 col_text "name",
								 col_array_int AS private_data,
								 col_array_text AS information
			 		FROM table_create_org
					WITH NO DATA;
--
SELECT * FROM table_create_ctas2_nodata;
--
CREATE TABLE table_create_ctas AS SELECT * FROM table_create_org;
--
SELECT * FROM table_create_ctas ORDER BY col_smallint;
--
CREATE TABLE table_create_ctas2
			 AS SELECT col_smallint id,
			 					 col_text "name",
								 col_array_int AS private_data,
								 col_array_text AS information
			 		FROM table_create_org
					WHERE col_smallint < 11;
--
SELECT * FROM table_create_ctas2 ORDER BY id;

--
-- CREATE TABLE WITH
--
CREATE TABLE patient (
	name		text,
	age			int4,
	dob			date
) WITH (fillfactor=40);

CREATE TABLE planetrip (
	origin		text,
	dest		text,
	day			date,
	depart		time
) WITH (user_catalog_table=true);

CREATE TABLE client (
	name		text,
	phonenum	int8,
	deadline	date
) WITH (oids=false);

--
-- CREATE TABLE SPLIT
--
CREATE TABLE tbl1 (
	a			int4 primary key
) SPLIT (INTO 20 TABLETS);

CREATE TABLE tbl1_5 (
	a       int4,
 	primary key(a asc)
) SPLIT (INTO 20 TABLETS);

CREATE TABLE tbl2 (
	a			int4,
	primary key (a asc)
) SPLIT AT VALUES ((4), (25), (100));

CREATE TABLE tbl3 (
	a			int4,
	primary key (a asc)
) SPLIT AT VALUES ((25), (100), (4));

CREATE TABLE tbl4 (
	a			int4,
	b			text,
	primary key (a asc, b)
) SPLIT AT VALUES ((1, 'c'), (1, 'cb'), (2, 'a'));

CREATE TABLE tbl5 (
	a			int4,
	b			text,
	primary key (b asc)
) SPLIT AT VALUES (('a'), ('aba'), ('ac'));

CREATE TABLE tbl6 (
	a			int4,
	b			text,
	primary key (b asc)
) SPLIT AT VALUES (('a'), (2, 'aba'), ('ac'));

CREATE TABLE tbl7 (
	a			int4,
	primary key (a asc)
) SPLIT AT VALUES (('a'), ('b'), ('c'));

CREATE TABLE tbl8 (
	a			text,
	primary key (a asc)
) SPLIT AT VALUES ((100), (1000), (10000));

CREATE TABLE tbl9 (
	a			int4,
	primary key (a hash)
) SPLIT AT VALUES ((100), (1000), (10000));

CREATE TEMPORARY TABLE tbl10 (
	a			int4 primary key
) SPLIT INTO 20 TABLETS;

CREATE TABLE tbl11 (
	a			int4,
	b			int4,
	c			int4,
	primary key (a asc, b desc)
) SPLIT AT VALUES ((-7, 1), (0, 0), (23, 4));

CREATE TABLE tbl12 (
	a			int4,
	b			text,
	primary key (b desc)
) SPLIT AT VALUES (('bienvenidos'), ('goodbye'), ('hello'), ('hola'));

CREATE TABLE tbl13 (
	a			text,
	b			date,
	c			time
) SPLIT INTO 9 TABLETS;

CREATE TABLE tbl14 (
	a			int4,
	primary key (a asc)
) SPLIT AT VALUES ((MINVALUE), (0), (MAXVALUE));

CREATE TABLE tbl15 (
	a			int4,
	b			int4,
	c			int4,
	primary key (a asc, b desc)
) SPLIT AT VALUES ((-10), (0, 0), (23, 4), (50));

-- This is invalid because split rows do not honor column b's ordering
CREATE TABLE tbl16(
  a int,
  b int,
  primary key(a asc, b asc)
) SPLIT AT VALUES((100), (200, 5), (200));

-- This is invalid because split rows do not honor column b's ordering
CREATE TABLE tbl16(
  a int,
  b int,
  primary key(a asc, b asc nulls first)
) SPLIT AT VALUES((100), (200, 5), (200));

-- This is invalid because split rows do not honor column b's ordering
CREATE TABLE tbl16(
  a int,
  b int,
  primary key(a asc, b asc nulls last)
) SPLIT AT VALUES((100), (200, 5), (200));

CREATE TABLE tbl16(
  a int,
  b int,
  primary key(a asc, b desc)
) SPLIT AT VALUES((100), (200), (200, 5));

CREATE TABLE tbl17(
  a int,
  b int,
  primary key(a asc, b desc nulls first)
) SPLIT AT VALUES((100), (200), (200, 5));

CREATE TABLE tbl18(
  a int,
  b int,
  primary key(a asc, b desc nulls last)
) SPLIT AT VALUES((100), (200), (200, 5));

-- This is invalid because we cannot have duplicate split rows
CREATE TABLE tbl19(
  a int,
  b int,
  primary key(a asc, b desc nulls last)
) SPLIT AT VALUES((100), (200, 5), (200, 5));

-- Test ordering on splitted tables
CREATE TABLE ordered_asc(
    k INT,
    PRIMARY KEY(k ASC)
) SPLIT AT VALUES((10), (20), (30));
INSERT INTO ordered_asc VALUES
    (5), (6), (16), (15), (25), (26), (36), (35), (46), (10), (20), (30);
EXPLAIN (COSTS OFF) SELECT * FROM ordered_asc ORDER BY k ASC;
SELECT * FROM ordered_asc ORDER BY k ASC;
EXPLAIN (COSTS OFF) SELECT * FROM ordered_asc ORDER BY k DESC;
SELECT * FROM ordered_asc ORDER BY k DESC;
EXPLAIN (COSTS OFF) SELECT k FROM ordered_asc WHERE k > 10 and k < 40 ORDER BY k DESC;
SELECT k FROM ordered_asc WHERE k > 10 and k < 40 ORDER BY k DESC;

CREATE TABLE ordered_desc(
    k INT,
    PRIMARY KEY(k DESC)
) SPLIT AT VALUES((30), (20), (10));
INSERT INTO ordered_desc VALUES
    (5), (6), (16), (15), (25), (26), (36), (35), (46), (10), (20), (30);
EXPLAIN (COSTS OFF) SELECT * FROM ordered_desc ORDER BY k ASC;
SELECT * FROM ordered_desc ORDER BY k ASC;
EXPLAIN (COSTS OFF) SELECT * FROM ordered_desc ORDER BY k DESC;
SELECT * FROM ordered_desc ORDER BY k DESC;
EXPLAIN (COSTS OFF) SELECT k FROM ordered_desc WHERE k > 10 and k < 40 ORDER BY k ASC;
SELECT k FROM ordered_desc WHERE k > 10 and k < 40 ORDER BY k ASC;
