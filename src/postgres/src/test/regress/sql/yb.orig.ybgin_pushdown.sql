--
-- Yugabyte-owned test on ybgin index with expression pushdown
--

\getenv abs_srcdir PG_ABS_SRCDIR
\set filename :abs_srcdir '/yb_commands/parameterized_query.sql'
\i :filename
\set P1 ':explain'
\set P2
\set explain 'EXPLAIN (costs off)'

CREATE TABLE gin_pushdown(
    id varchar(64) not null,
    guid varchar(64) not null unique,
    status varchar(64),
    json_content jsonb not null,
    primary key (id)
);

-- gin index
CREATE INDEX gin_pushdown_json_content_idx ON gin_pushdown USING ybgin (json_content jsonb_path_ops);

INSERT INTO gin_pushdown
  SELECT x::text, x::text, x::text, ('{"refs": [{"val":"'||x||'"}, {"val":"'||x+1||'"}]}')::jsonb
  FROM generate_series (1, 10) x;

-- Find rows using gin index
SELECT $$
:P SELECT * FROM gin_pushdown WHERE json_content @> '{"refs": [{"val":"9"}]}';
$$ AS query \gset
\i :run_query

-- Use pushdown filter
SELECT $$
:P SELECT * FROM gin_pushdown WHERE json_content @> '{"refs": [{"val":"9"}]}' AND status <> '9';
$$ AS query \gset
\i :run_query
SELECT $$
:P SELECT * FROM gin_pushdown WHERE json_content @> '{"refs": [{"val":"9"}]}' AND status = '9';
$$ AS query \gset
\i :run_query

-- Pushdown filter that may seem to be pushed with the index scan, however ybgin index
-- does not store the indexed value, hence filter goes to the main relation
SELECT $$
:P SELECT * FROM gin_pushdown WHERE json_content @> '{"refs": [{"val":"9"}]}' AND json_content->'refs'->0->'val' <> '"9"';
$$ AS query \gset
\i :run_query
SELECT $$
:P SELECT * FROM gin_pushdown WHERE json_content @> '{"refs": [{"val":"9"}]}' AND json_content->'refs'->0->'val' = '"9"';
$$ AS query \gset
\i :run_query

-- Volatile expressions cannot be pushed to the index.
SELECT $$
:P SELECT * FROM gin_pushdown WHERE json_content @> '{"refs": [{"val":"9"}]}' AND random() > 2.0;
$$ AS query \gset
\i :run_query
SELECT $$
:P SELECT * FROM gin_pushdown WHERE json_content @> '{"refs": [{"val":"9"}]}' AND random() < 2.0;
$$ AS query \gset
\i :run_query

-- Find row using regular index
SELECT $$
:P SELECT * FROM gin_pushdown WHERE guid = '9';
$$ AS query \gset
\i :run_query

-- Use pushdown filter
SELECT $$
:P SELECT * FROM gin_pushdown WHERE guid = '9' AND status <> '9';
$$ AS query \gset
\i :run_query
SELECT $$
:P SELECT * FROM gin_pushdown WHERE guid = '9' AND status = '9';
$$ AS query \gset
\i :run_query

-- Pushdown filter that goes with the index scan, since json_content is included
SELECT $$
:P SELECT * FROM gin_pushdown WHERE guid = '9' AND json_content->'refs'->0->'val' <> '"9"';
$$ AS query \gset
\i :run_query
SELECT $$
:P SELECT * FROM gin_pushdown WHERE guid = '9' AND json_content->'refs'->0->'val' = '"9"';
$$ AS query \gset
\i :run_query

-- Volatile expressions cannot be pushed to the index.
SELECT $$
:P SELECT * FROM gin_pushdown WHERE guid = '9' AND random() > 2.0;
$$ AS query \gset
\i :run_query
SELECT $$
:P SELECT * FROM gin_pushdown WHERE guid = '9' AND random() < 2.0;
$$ AS query \gset
\i :run_query

-- Cleanup
DROP TABLE gin_pushdown;
