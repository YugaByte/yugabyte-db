---
title: WITH clause—SQL syntax and semantics
linkTitle: WITH clause—SQL syntax and semantics
headerTitle: WITH clause—SQL syntax and semantics
description: This section specifies the syntax and semantics of the WITH clause
menu:
  latest:
    identifier: with-clause-syntax-semantics
    parent: with-clause
    weight: 20
isTocNested: true
showAsideToc: true
---

## Syntax

The [`with_clause_substatement_defn`](../../../syntax_resources/grammar_diagrams/#with-clause-substatement-defn) diagram is reproduced from the section that describes the [`SELECT` statement](../../../the-sql-language/statements/dml_select/).

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#grammar" class="nav-link active" id="grammar-tab" data-toggle="tab" role="tab" aria-controls="grammar" aria-selected="true">
      <i class="fas fa-file-alt" aria-hidden="true"></i>
      Grammar
    </a>
  </li>
  <li>
    <a href="#diagram" class="nav-link" id="diagram-tab" data-toggle="tab" role="tab" aria-controls="diagram" aria-selected="false">
      <i class="fas fa-project-diagram" aria-hidden="true"></i>
      Diagram
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="grammar" class="tab-pane fade show active" role="tabpanel" aria-labelledby="grammar-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/with_clause_substatement_defn.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/with_clause_substatement_defn.diagram.md" /%}}
  </div>
</div>

## Semantics

The `WITH` clause lets you name a SQL statement which might be one of [`SELECT`](../../../the-sql-language/statements/dml_select/), [`VALUES`](../../../the-sql-language/statements/dml_values/), [`INSERT`](../../../the-sql-language/statements/dml_insert/), [`UPDATE`](../../../the-sql-language/statements/dml_update/), or [`DELETE`](../../../the-sql-language/statements/dml_delete/). You can then refer to the statement by name (just as if it were a schema-level view that names a `SELECT` statement) in a subsequent `WITH` clause substatement definition or in the statements final section. A very common use of the data-changing statements in the `WITH` clause is when they have a `RETURNING` clause. Then when you later refer to that statement by name, it behaves the same as if it were a named `SELECT` statement.

The uniqueness scope for the name of the substatement definition is the relation names defined in a particular `WITH` clause the SQL statement. You can define column aliases compactly in the optional parenthesized list that follows the name of the substatement, just as you can with a schema-level view. See the section [Example where a substatement defined in the WITH clause itself has a WITH clause](#example-where-a-substatement-defined-in-the-with-clause-itself-has-a-with-clause).

Notice that the `WITH` clause is legal in the `SELECT` statement, in each of the kinds of data-changing statements, but not in the `VALUES` statement. See the syntax diagrams for [`SELECT`](../../../syntax_resources/grammar_diagrams/#select), [`VALUES`](../../../syntax_resources/grammar_diagrams/#values), [`INSERT`](../../../syntax_resources/grammar_diagrams/#insert), [`UPDATE`](../../../syntax_resources/grammar_diagrams/#update), and [`DELETE`](../../../syntax_resources/grammar_diagrams/#delete).

### Example that uses three data-changing substatements and a SELECT substatement in the WITH clause

First, create some test tables and inspect the contents.

```plpgsql
set client_min_messages = warning;

do $body$
declare
  table_names constant text[] := array['t1', 't2', 't3'];
  drop_table constant text := '
    drop table if exists ? cascade';
  create_table constant text := '
    create table ?(k int primary key, v int)';
  insert_table constant text := '
    insert into ?(k, v)
    select g.v, g.v*2 from generate_series($1, $2) as g(v)';

  t text not null := '';
  i int not null := 1;
  n constant int := 2;
begin
  foreach t in array table_names loop
    execute replace(drop_table,   '?', t);
    execute replace(create_table, '?', t);
    execute replace(insert_table, '?', t) using i, i + n;
    i := i + n + 1;
  end loop;
end;
$body$;

select 't1'::text as name, k, v from t1
union all
select 't2'::text as name, k, v from t2
union all
select 't3'::text as name, k, v from t3
order by 1, 2;
```

This is the result:

```
 name | k | v  
------+---+----
 t1   | 1 |  2
 t1   | 2 |  4
 t1   | 3 |  6
 t2   | 4 |  8
 t2   | 5 | 10
 t2   | 6 | 12
 t3   | 7 | 14
 t3   | 8 | 16
 t3   | 9 | 18
```

Now execute the example query. Notice that the `WITH` clause defines an `INSERT` substatement, an `UPDATE` substatement, a `DELETE` substatement, and a `SELECT` substatement. Each of the data-changing substatements has a `RETURNING` clause; and the `SELECT` substatement accesses the unions returned by each of these.

```plpgsql
with 
  i as (
    insert into t1(k, v) values (21, 17), (31, 42) returning k, v),
    
  u as (
    update t2 set v = 99 where k in (4, 5) returning k, v),
    
  d as (
    delete from t3 where k in (8, 9) returning k, v),
    
  s as (
    select 'inserted into t1'::text as action, k, v from i
    union all
    select 'udated in t2'::text     as action, k, v from u
    union all
    select 'deleted from t3'::text  as action, k, v from d)

select action, k, v from s order by k;
```

This is the result:

```
      action      | k  | v  
------------------+----+----
 udated in t2     |  4 | 99
 udated in t2     |  5 | 99
 deleted from t3  |  8 | 16
 deleted from t3  |  9 | 18
 inserted into t1 | 21 | 17
 inserted into t1 | 31 | 42
```

Check that the result is as expected in the affected tables, _"t1"_, _"t2"_, and _"t3"_:

```plpgsql
select 't1'::text as name, k, v from t1
union all
select 't2'::text as name, k, v from t2
union all
select 't3'::text as name, k, v from t3
order by 1, 2;
```

This is the result:

```
 name | k  | v  
------+----+----
 t1   |  1 |  2
 t1   |  2 |  4
 t1   |  3 |  6
 t1   | 21 | 17
 t1   | 31 | 42
 t2   |  4 | 99
 t2   |  5 | 99
 t2   |  6 | 12
 t3   |  7 | 14
```

### Example that uses a VALUES substatements and a SELECT substatement in the WITH clause

First, clear out the data from table _"t1"_ from the previous demonstration.

```plpgsql
truncate table t1;
```

Now use a `WITH` clause as part of an `INSERT` statement.

```plpgsql
with
  a1 as (
    select g.v as k, g.v*2 as v
    from generate_series(1, 3) as g(v)),

  a2 as (
    values (10, 17), (11, 42), (12, 99))

insert into t1(k, v)
select k, v from a1
union all
select column1 as k, column2 as v from a2;

select k, v from t1 order by k;
```

This is the result:

```
 k  | v  
----+----
  1 |  2
  2 |  4
  3 |  6
 10 | 17
 11 | 42
 12 | 99
```

### Example where a substatement defined in the WITH clause itself has a WITH clause

The following SQL statement uses _two_ `WITH` clauses. Each defines its own scope for the names of the substatements that they define. Notice that the name _"a2"_ is defined in three different scopes: schema scope; the scope of the outer `WITH` clause; and the scope of the inner `WITH` clause that the outer one begins with.

The outer `WITH` clause simply cannot see the names that are defined in the inner `WITH` clause. But each of these `WITH` clauses can see a schema-level relation with a name that collides with a name defined in that `WITH` clause by qualifying it with the schema name. Column names are always defined within the scope of the relation that contains them.

The example assumes that a user called _"u1"_ exists and that a database called _"demo"_ exists. Change these names in the example to match the names that are available to you.

```plpgsql
\c demo u1
set client_min_messages = warning;
drop table if exists colliding cascade;

create table u1.colliding(colliding text primary key);
insert into u1.colliding(colliding) values ('goodbye'), ('world');

with
  colliding(colliding) as (
    with
      a(colliding) as (select 'Hello'),
      colliding(colliding) as (select colliding from u1.colliding where colliding like 'w%')
    select ((select colliding from a)||' '||(select colliding from colliding))
    )

select
  (select colliding from u1.colliding where colliding like 'g%')||'—'||colliding
  as colliding
from colliding;
```

This is the result:

```
      colliding      
---------------------
 goodbye—Hello world
```
