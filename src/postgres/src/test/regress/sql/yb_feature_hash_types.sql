--
-- YB_FEATURE Testsuite
--   An introduction on whether or not a feature is supported in YugaByte.
--   This test suite does not go in depth for each command.
--
-- Numeric Types
CREATE TABLE ft_h_tab_smallint (feature_col SMALLINT PRIMARY KEY);
CREATE TABLE ft_h_tab_integer (feature_col INTEGER PRIMARY KEY);
CREATE TABLE ft_h_tab_bigint (feature_col BIGINT PRIMARY KEY);
CREATE TABLE ft_h_tab_real (feature_col REAL PRIMARY KEY);
CREATE TABLE ft_h_tab_double_precision (feature_col DOUBLE PRECISION PRIMARY KEY);
CREATE TABLE ft_h_tab_decimal (feature_col DECIMAL PRIMARY KEY);
CREATE TABLE ft_h_tab_numeric (feature_col NUMERIC PRIMARY KEY);
CREATE TABLE ft_h_tab_smallserial (feature_col SMALLSERIAL PRIMARY KEY);
CREATE TABLE ft_h_tab_serial (feature_col SERIAL PRIMARY KEY);
CREATE TABLE ft_h_tab_bigserial (feature_col BIGSERIAL PRIMARY KEY);
--
-- Monetary Types
CREATE TABLE ft_h_tab_money (feature_col MONEY PRIMARY KEY);
--
-- Character Types
CREATE TABLE ft_h_tab_character_varying (feature_col CHARACTER VARYING(10) PRIMARY KEY);
CREATE TABLE ft_h_tab_varchar (feature_col VARCHAR(10) PRIMARY KEY);
CREATE TABLE ft_h_tab_character (feature_col CHARACTER(10) PRIMARY KEY);
CREATE TABLE ft_h_tab_char (feature_col CHAR(10) PRIMARY KEY);
CREATE TABLE ft_h_tab_text (feature_col TEXT PRIMARY KEY);
--
-- Binary Types
CREATE TABLE ft_h_tab_bytea (feature_col BYTEA PRIMARY KEY);
--
-- Date Time Types
CREATE TABLE ft_h_tab_timestamp (feature_col TIMESTAMP(2) PRIMARY KEY);
CREATE TABLE ft_h_tab_timestamp_with_time_zone (feature_col TIMESTAMP WITH TIME ZONE PRIMARY KEY);
CREATE TABLE ft_h_tab_date (feature_col DATE PRIMARY KEY);
CREATE TABLE ft_h_tab_time (feature_col TIME(2) PRIMARY KEY);
CREATE TABLE ft_h_tab_time_with_time_zone (feature_col TIME(2) WITH TIME ZONE PRIMARY KEY);
CREATE TABLE ft_h_tab_interval_ym (feature_col INTERVAL YEAR TO MONTH PRIMARY KEY);
CREATE TABLE ft_h_tab_interval_ds (feature_col INTERVAL DAY TO SECOND(2) PRIMARY KEY);
--
-- Boolean Type
CREATE TABLE ft_h_tab_bool (feature_col BOOLEAN PRIMARY KEY);
--
-- Enumerated Types
CREATE TYPE feature_h_enum AS ENUM('one', 'two', 'three');
CREATE TABLE ft_h_tab_enum (feature_col feature_h_enum PRIMARY KEY);
--
-- Geometric Types
CREATE TABLE ft_h_tab_point (feature_col POINT PRIMARY KEY);
CREATE TABLE ft_h_tab_line (feature_col LINE PRIMARY KEY);
CREATE TABLE ft_h_tab_lseg (feature_col LSEG PRIMARY KEY);
CREATE TABLE ft_h_tab_box (feature_col BOX PRIMARY KEY);
CREATE TABLE ft_h_tab_path (feature_col PATH PRIMARY KEY);
CREATE TABLE ft_h_tab_polygon (feature_col POLYGON PRIMARY KEY);
CREATE TABLE ft_h_tab_circle (feature_col CIRCLE PRIMARY KEY);
--
-- Network Address Types
CREATE TABLE ft_h_tab_cidr (feature_col CIDR PRIMARY KEY);
CREATE TABLE ft_h_tab_inet (feature_col INET PRIMARY KEY);
CREATE TABLE ft_h_tab_macaddr (feature_col MACADDR PRIMARY KEY);
CREATE TABLE ft_h_tab_macaddr8 (feature_col MACADDR8 PRIMARY KEY);
--
-- Bit String Types
CREATE TABLE ft_h_tab_bit (feature_col BIT PRIMARY KEY);
CREATE TABLE ft_h_tab_bit_varying (feature_col BIT VARYING(10) PRIMARY KEY);
--
-- Text Search Types
CREATE TABLE ft_h_tab_tsvector (feature_col TSVECTOR PRIMARY KEY);
CREATE TABLE ft_h_tab_tsquery (feature_col TSQUERY PRIMARY KEY);
--
-- UUID Type
CREATE TABLE ft_h_tab_uuid (feature_col UUID PRIMARY KEY);
--
-- XML Type
CREATE TABLE ft_h_tab_xml (feature_col XML PRIMARY KEY);
--
-- Arrays
CREATE TABLE ft_h_tab_array_int (feature_col INTEGER[] PRIMARY KEY);
CREATE TABLE ft_h_tab_array_text (feature_col TEXT[] PRIMARY KEY);
--
-- Composite Types
CREATE TYPE feature_h_struct AS(id INTEGER, name TEXT);
CREATE TABLE ft_h_tab_struct (feature_col feature_h_struct PRIMARY KEY);
--
-- JSON Types
CREATE TABLE ft_h_tab_json (feature_col JSON PRIMARY KEY);
--
-- Range Types
CREATE TYPE feature_h_range AS RANGE(subtype=INTEGER);
CREATE TABLE ft_h_tab_range (feature_col feature_h_range PRIMARY KEY);
--
-- Domain Types
CREATE DOMAIN feature_h_domain AS INTEGER CHECK (VALUE > 0);
CREATE TABLE ft_h_tab_domain (feature_col feature_h_domain PRIMARY KEY);
--
-- Object Identifier Types
CREATE TABLE ft_h_tab_oid (feature_col OID PRIMARY KEY);
CREATE TABLE ft_h_tab_regproc (feature_col REGPROC PRIMARY KEY);
CREATE TABLE ft_h_tab_regprocedure (feature_col REGPROCEDURE PRIMARY KEY);
CREATE TABLE ft_h_tab_regoper (feature_col REGOPER PRIMARY KEY);
CREATE TABLE ft_h_tab_regoperator (feature_col REGOPERATOR PRIMARY KEY);
CREATE TABLE ft_h_tab_regclass (feature_col REGCLASS PRIMARY KEY);
CREATE TABLE ft_h_tab_regtype (feature_col REGTYPE PRIMARY KEY);
CREATE TABLE ft_h_tab_regrole (feature_col REGROLE PRIMARY KEY);
CREATE TABLE ft_h_tab_regnamespace (feature_col REGNAMESPACE PRIMARY KEY);
CREATE TABLE ft_h_tab_regconfig (feature_col REGCONFIG PRIMARY KEY);
CREATE TABLE ft_h_tab_regdictionary (feature_col REGDICTIONARY PRIMARY KEY);
CREATE TABLE ft_h_tab_xid (feature_col XID PRIMARY KEY);
CREATE TABLE ft_h_tab_cid (feature_col CID PRIMARY KEY);
CREATE TABLE ft_h_tab_tid (feature_col TID PRIMARY KEY);
--
-- pg_lsn Type
CREATE TABLE ft_h_tab_pg_lsn (feature_col PG_LSN PRIMARY KEY);
--
-- Pseudo-Types
CREATE TABLE ft_h_tab_any (feature_col ANY PRIMARY KEY);
CREATE TABLE ft_h_tab_anyelement (feature_col ANYELEMENT PRIMARY KEY);
CREATE TABLE ft_h_tab_anyarray (feature_col ANYARRAY PRIMARY KEY);
CREATE TABLE ft_h_tab_anynonarray (feature_col ANYNONARRAY PRIMARY KEY);
CREATE TABLE ft_h_tab_anyenum (feature_col ANYENUM PRIMARY KEY);
CREATE TABLE ft_h_tab_anyrange (feature_col ANYRANGE PRIMARY KEY);
CREATE TABLE ft_h_tab_cstring (feature_col CSTRING PRIMARY KEY);
CREATE TABLE ft_h_tab_internal (feature_col INTERNAL PRIMARY KEY);
CREATE TABLE ft_h_tab_language_handler (feature_col LANGUAGE_HANDLER PRIMARY KEY);
CREATE TABLE ft_h_tab_fdw_handler (feature_col FDW_HANDLER PRIMARY KEY);
CREATE TABLE ft_h_tab_index_am_handler (feature_col INDEX_AM_HANDLER PRIMARY KEY);
CREATE TABLE ft_h_tab_tsm_handler (feature_col TSM_HANDLER PRIMARY KEY);
CREATE TABLE ft_h_tab_record (feature_col RECORD PRIMARY KEY);
CREATE TABLE ft_h_tab_trigger (feature_col TRIGGER PRIMARY KEY);
CREATE TABLE ft_h_tab_event_trigger (feature_col EVENT_TRIGGER PRIMARY KEY);
CREATE TABLE ft_h_tab_pg_ddl_command (feature_col PG_DDL_COMMAND PRIMARY KEY);
CREATE TABLE ft_h_tab_void (feature_col VOID PRIMARY KEY);
-- TODO(jason): uncomment when issue #1975 is closed.
-- CREATE TABLE ft_h_tab_unknown (feature_col UNKNOWN);
CREATE TABLE ft_h_tab_opaque (feature_col OPAQUE PRIMARY KEY);
