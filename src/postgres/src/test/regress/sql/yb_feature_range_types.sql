--
-- YB_FEATURE_PARTITIONING Testsuite
--   An introduction on whether or not a feature is supported in YugaByte.
--   This test suite does not go in depth for each command.
--
-- Numeric Types
CREATE TABLE ft_r_tab_smallint (feature_col SMALLINT, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_integer (feature_col INTEGER, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_bigint (feature_col BIGINT, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_real (feature_col REAL, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_double_precision (feature_col DOUBLE PRECISION, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_decimal (feature_col DECIMAL, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_numeric (feature_col NUMERIC, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_smallserial (feature_col SMALLSERIAL, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_serial (feature_col SERIAL, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_bigserial (feature_col BIGSERIAL, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
--
-- Monetary Types
CREATE TABLE ft_r_tab_money (feature_col MONEY, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('$100.99'));
--
-- Character Types
CREATE TABLE ft_r_tab_character_varying (feature_col CHARACTER VARYING(10), PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('text value'));
CREATE TABLE ft_r_tab_varchar (feature_col VARCHAR(10), PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('text value'));
CREATE TABLE ft_r_tab_character (feature_col CHARACTER(10), PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('text value'));
CREATE TABLE ft_r_tab_char (feature_col CHAR(10), PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('text value'));
CREATE TABLE ft_r_tab_text (feature_col TEXT, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('text value'));
--
-- Binary Types
CREATE TABLE ft_r_tab_bytea (feature_col BYTEA, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('text value'));
--
-- Date Time Types
CREATE TABLE ft_r_tab_timestamp (feature_col TIMESTAMP(2), PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('1234-01-30 07:08:09'));
CREATE TABLE ft_r_tab_timestamp_with_time_zone (feature_col TIMESTAMP WITH TIME ZONE, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('1234-01-30 07:08:09+06'));
CREATE TABLE ft_r_tab_date (feature_col DATE, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('1234-01-30'));
CREATE TABLE ft_r_tab_time (feature_col TIME(2), PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('07:08:09'));
CREATE TABLE ft_r_tab_time_with_time_zone (feature_col TIME(2) WITH TIME ZONE, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_interval_ym (feature_col INTERVAL YEAR TO MONTH, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_interval_ds (feature_col INTERVAL DAY TO SECOND(2), PRIMARY KEY(feature_col ASC));
--
-- Boolean Type
CREATE TABLE ft_r_tab_bool (feature_col BOOLEAN, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('true'));
--
-- Enumerated Types
CREATE TYPE feature_r_enum AS ENUM('one', 'two', 'three');
CREATE TABLE ft_r_tab_enum (feature_col feature_r_enum, PRIMARY KEY(feature_col ASC));
--
-- Geometric Types
CREATE TABLE ft_r_tab_point (feature_col POINT, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_line (feature_col LINE, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_lseg (feature_col LSEG, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_box (feature_col BOX, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_path (feature_col PATH, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_polygon (feature_col POLYGON, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_circle (feature_col CIRCLE, PRIMARY KEY(feature_col ASC));
--
-- Network Address Types
CREATE TABLE ft_r_tab_cidr (feature_col CIDR, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_inet (feature_col INET, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_macaddr (feature_col MACADDR, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_macaddr8 (feature_col MACADDR8, PRIMARY KEY(feature_col ASC));
--
-- Bit String Types
CREATE TABLE ft_r_tab_bit (feature_col BIT, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_bit_varying (feature_col BIT VARYING(10), PRIMARY KEY(feature_col ASC));
--
-- Text Search Types
CREATE TABLE ft_r_tab_tsvector (feature_col TSVECTOR, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_tsquery (feature_col TSQUERY, PRIMARY KEY(feature_col ASC));
--
-- UUID Type
CREATE TABLE ft_r_tab_uuid (feature_col UUID, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('12345678-1234-5678-1234-567812345678'));
--
-- XML Type
CREATE TABLE ft_r_tab_xml (feature_col XML, PRIMARY KEY(feature_col ASC));
--
-- Arrays
CREATE TABLE ft_r_tab_array_int (feature_col INTEGER[], PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_array_text (feature_col TEXT[], PRIMARY KEY(feature_col ASC));
--
-- Composite Types
CREATE TYPE feature_r_struct AS(id INTEGER, name TEXT);
CREATE TABLE ft_r_tab_struct (feature_col feature_r_struct, PRIMARY KEY(feature_col ASC));
--
-- JSON Types
CREATE TABLE ft_r_tab_json (feature_col JSON, PRIMARY KEY(feature_col ASC));
--
-- Range Types
CREATE TYPE feature_r_range AS RANGE(subtype=INTEGER);
CREATE TABLE ft_r_tab_range (feature_col feature_r_range, PRIMARY KEY(feature_col ASC));
--
-- Domain Types
CREATE DOMAIN feature_r_domain AS INTEGER CHECK (VALUE > 0);
CREATE TABLE ft_r_tab_domain (feature_col feature_r_domain, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
--
-- Object Identifier Types
CREATE TABLE ft_r_tab_oid (feature_col OID, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regproc (feature_col REGPROC, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regprocedure (feature_col REGPROCEDURE, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100100));
CREATE TABLE ft_r_tab_regoper (feature_col REGOPER, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regoperator (feature_col REGOPERATOR, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regclass (feature_col REGCLASS, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regtype (feature_col REGTYPE, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regrole (feature_col REGROLE, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regnamespace (feature_col REGNAMESPACE, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regconfig (feature_col REGCONFIG, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_regdictionary (feature_col REGDICTIONARY, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES ((100));
CREATE TABLE ft_r_tab_xid (feature_col XID, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_cid (feature_col CID, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_tid (feature_col TID, PRIMARY KEY(feature_col ASC));
--
-- pg_lsn Type
CREATE TABLE ft_r_tab_pg_lsn (feature_col PG_LSN, PRIMARY KEY(feature_col ASC))
	SPLIT AT VALUES (('1/2345678'));
--
-- Pseudo-Types
CREATE TABLE ft_r_tab_any (feature_col ANY, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_anyelement (feature_col ANYELEMENT, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_anyarray (feature_col ANYARRAY, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_anynonarray (feature_col ANYNONARRAY, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_anyenum (feature_col ANYENUM, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_anyrange (feature_col ANYRANGE, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_cstring (feature_col CSTRING, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_internal (feature_col INTERNAL, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_language_handler (feature_col LANGUAGE_HANDLER, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_fdw_handler (feature_col FDW_HANDLER, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_index_am_handler (feature_col INDEX_AM_HANDLER, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_tsm_handler (feature_col TSM_HANDLER, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_record (feature_col RECORD, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_trigger (feature_col TRIGGER, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_event_trigger (feature_col EVENT_TRIGGER, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_pg_ddl_command (feature_col PG_DDL_COMMAND, PRIMARY KEY(feature_col ASC));
CREATE TABLE ft_r_tab_void (feature_col VOID, PRIMARY KEY(feature_col ASC));
-- TODO(jason): uncomment when issue #1975 is closed.
-- CREATE TABLE ft_r_tab_unknown (feature_col UNKNOWN);
CREATE TABLE ft_r_tab_opaque (feature_col OPAQUE, PRIMARY KEY(feature_col ASC));
