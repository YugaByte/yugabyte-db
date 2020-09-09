---
title: EXPLAIN statement [YCQL]
headerTitle: EXPLAIN
linkTitle: EXPLAIN
description: Use the EXPLAIN statement to show the execution plan for a YCQL statement.
menu:
  latest:
    parent: api-cassandra
    weight: 1320
aliases:
  - /latest/api/ycql/explain/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `EXPLAIN` statement to show the execution plan for a statement.

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="213" height="125" viewbox="0 0 213 125"><path class="connector" d="M0 22h15m72 0h30m54 0h27m-91 25q0 5 5 5h5m61 0h5q5 0 5-5m-81 30q0 5 5 5h5m54 0h12q5 0 5-5m-86-55q5 0 5 5v80q0 5 5 5h5m56 0h10q5 0 5-5v-80q0-5 5-5m5 0h15"/><polygon points="0,29 5,22 0,15" style="fill:black;stroke-width:0"/><rect class="literal" x="15" y="5" width="72" height="25" rx="7"/><text class="text" x="25" y="22">EXPLAIN</text><a xlink:href="../grammar_diagrams#select"><rect class="rule" x="117" y="5" width="54" height="25"/><text class="text" x="127" y="22">select</text></a><a xlink:href="../grammar_diagrams#update"><rect class="rule" x="117" y="35" width="61" height="25"/><text class="text" x="127" y="52">update</text></a><a xlink:href="../grammar_diagrams#insert"><rect class="rule" x="117" y="65" width="54" height="25"/><text class="text" x="127" y="82">insert</text></a><a xlink:href="../grammar_diagrams#delete"><rect class="rule" x="117" y="95" width="56" height="25"/><text class="text" x="127" y="112">delete</text></a><polygon points="209,29 213,29 213,15 209,15" style="fill:black;stroke-width:0"/></svg>

### Grammar

```
explain ::= EXPLAIN { select | update | insert | delete }
```


## Semantics

Where the target statement is one of the following: [SELECT](../dml_select), [UPDATE](../dml_update), [INSERT](../dml_insert), or [DELETE](../dml_delete).

## Examples
Create the keyspace, tables and indexes.

### Setup Table and indexes
```CQL
cqlsh> CREATE KEYSPACE IF NOT EXISTS imdb;
cqlsh> CREATE TABLE IF NOT EXISTS imdb.movie_stats (
           movie_name text,
           movie_genre text,
           user_name text,
           user_rank int,
           last_watched timestamp,
           PRIMARY KEY (movie_genre, movie_name, user_name)
    ) WITH transactions = { 'enabled' : true };
cqlsh> CREATE INDEX IF NOT EXISTS most_watched_by_year
      ON imdb.movie_stats((movie_genre, last_watched), movie_name, user_name)
      INCLUDE(user_rank);
cqlsh> CREATE INDEX IF NOT EXISTS best_rated
      ON imdb.movie_stats((user_rank, movie_genre), movie_name, user_name)
      INCLUDE(last_watched);
```

Insert some rows.
```CQL
cqlsh> USE imdb;
cqlsh:imdb> INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank, last_watched)
            VALUES ('m1', 'g1', 'u1', 5, '2019-01-18');
cqlsh:imdb> INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank, last_watched)
            VALUES ('m2', 'g2', 'u1', 4, '2019-01-17');
cqlsh:imdb> INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank, last_watched)
            VALUES ('m3', 'g1', 'u2', 5, '2019-01-18');
cqlsh:imdb> INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank, last_watched)
            VALUES ('m4', 'g1', 'u1', 2, '2019-02-27');
```
### Explain query plans
If movie_genre, or movie_genre & movie_name, or movie_genre & movie_name & user_name are specified, the query should be served efficiently from the primary table.

```CQL
cqlsh:imdb> EXPLAIN SELECT *
            FROM movie_stats
            WHERE movie_genre = 'g1';

QUERY PLAN
----------------------------------------
 Range Scan on imdb.movie_stats
   Key Conditions: (movie_genre = 'g1')
```
If movie_genre & last_watched are specified, then the query should be served efficiently from the `most_watched_by_year` index.

```CQL
cqlsh:imdb> EXPLAIN SELECT *
            FROM movie_stats
            WHERE movie_genre = 'g1' and last_watched='2019-02-27';

QUERY PLAN
--------------------------------------------------------------------------
 Index Only Scan using imdb.most_watched_by_year on imdb.movie_stats
   Key Conditions: (movie_genre = 'g1') AND (last_watched = '2019-02-27')

```

If user_rank and movie_genre are specified then the query should be served efficiently from the `best_rated` index.

```CQL
cqlsh:imdb> EXPLAIN SELECT *
            FROM movie_stats
            WHERE movie_genre = 'g2' and user_rank=5;

QUERY PLAN
--------------------------------------------------------------
 Index Only Scan using imdb.best_rated on imdb.movie_stats
   Key Conditions: (user_rank = '5') AND (movie_genre = 'g2')

```
Create non-covering index.
```CQL
cqlsh:imdb> DROP INDEX best_rated;
cqlsh:imdb> CREATE INDEX IF NOT EXISTS best_rated
            ON imdb.movie_stats((user_rank, movie_genre), movie_name, user_name);
```
2-Step select. Using Index Scan as opposed to Index Only Scan.
```CQL
cqlsh:imdb> EXPLAIN SELECT *
            FROM movie_stats
            WHERE movie_genre = 'g2' and user_rank=5;

 QUERY PLAN
--------------------------------------------------------------
 Index Scan using imdb.best_rated on imdb.movie_stats
   Key Conditions: (user_rank = '5') AND (movie_genre = 'g2')
```

{{< note title="Note" >}}

**INDEX SCAN**: Filters rows using the index and then fetches the columns from the main table.

**INDEX ONLY SCAN**: Returns results by only consulting the index.

{{< /note >}}


### Other EXPLAIN SELECT types
`QLName()` for these expressions is not supported.
```CQL
cqlsh:imdb> EXPLAIN SELECT * FROM movie_stats where movie_genre in ('g1', 'g2');

 QUERY PLAN
-------------------------------------------
 Range Scan on imdb.movie_stats
   Key Conditions: (movie_genre IN 'expr')
```


```CQL
cqlsh:imdb> EXPLAIN SELECT COUNT(*) FROM movie_stats  WHERE movie_genre = 'g2' and user_rank=5;

 QUERY PLAN
--------------------------------------------------------------------
 Aggregate
   ->  Index Only Scan using imdb.best_rated on imdb.movie_stats
         Key Conditions: (user_rank = '5') AND (movie_genre = 'g2')
```


```CQL
cqlsh:imdb> EXPLAIN SELECT * FROM movie_stats  WHERE movie_genre = 'g2' and user_rank = 5 LIMIT 5;

 QUERY PLAN
--------------------------------------------------------------------
 Limit
   ->  Index Only Scan using imdb.best_rated on imdb.movie_stats
         Key Conditions: (user_rank = '5') AND (movie_genre = 'g2')
```
### INSERT example

```CQL
cqlsh:imdb> EXPLAIN INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank, last_watched)
            VALUES ('m4', 'g1', 'u1', 2, '2019-02-27');

 QUERY PLAN
----------------------------
 Insert on imdb.movie_stats
```
### DELETE examples

```CQL
cqlsh:imdb> explain delete from movie_stats  where movie_genre = 'g1' and movie_name = 'm1';

 QUERY PLAN
----------------------------------------------
 Delete on imdb.movie_stats
   ->  Range Scan on imdb.movie_stats
         Key Conditions: (movie_genre = 'g1')
         Filter: (movie_name = 'm1')
```
```CQL
cqlsh:imdb> explain delete from movie_stats  where movie_genre = 'g1';

 QUERY PLAN
----------------------------------------------
 Delete on imdb.movie_stats
   ->  Range Scan on imdb.movie_stats
         Key Conditions: (movie_genre = 'g1')
```
### UPDATE example

```CQL
cqlsh:imdb> EXPLAIN UPDATE movie_stats SET user_rank = 1 WHERE movie_name = 'm1' and movie_genre = 'g1' and user_name = 'u1';

 QUERY PLAN
---------------------------------------------------------------------------------------------
 Update on imdb.movie_stats
   ->  Primary Key Lookup on imdb.movie_stats
         Key Conditions: (movie_genre = 'g1') AND (movie_name = 'm1') AND (user_name = 'u1')
```

## See also

- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)
- [`UPDATE`](../dml_update)
- [`DELETE`](../dml_delete)
