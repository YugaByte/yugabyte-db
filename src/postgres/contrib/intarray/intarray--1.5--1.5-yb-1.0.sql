/* contrib/intarray/intarray--1.5--1.5-yb-1.0.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION intarray UPDATE TO '1.5-yb-1.0'" to load this file. \quit

/*
 * Translate gin operator class to ybgin operator class.  Ignore gist because
 * the ybgist index access method does not exist, yet.
 */

/*
 * Adopted from contrib/intarray/intarray--1.2.sql, reflecting the state of
 * gin__int_ops at version 1.5: the deprecated @ and ~ operators (strategies 13
 * and 14) were dropped in intarray--1.4--1.5.sql, so they are omitted here.
 */

CREATE OPERATOR CLASS gin__int_ops
FOR TYPE _int4 USING ybgin
AS
	OPERATOR	3	&&,
	OPERATOR	6	= (anyarray, anyarray),
	OPERATOR	7	@>,
	OPERATOR	8	<@,
	OPERATOR	20	@@ (_int4, query_int),
	FUNCTION	1	btint4cmp (int4, int4),
	FUNCTION	2	ginarrayextract (anyarray, internal, internal),
	FUNCTION	3	ginint4_queryextract (_int4, internal, int2, internal, internal, internal, internal),
	FUNCTION	4	ginint4_consistent (internal, int2, _int4, int4, internal, internal, internal, internal),
	STORAGE		int4;
