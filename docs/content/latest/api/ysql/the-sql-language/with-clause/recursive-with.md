---
title: WITH clause recursive substatement—SQL syntax and semantics
linkTitle: recursive WITH
headerTitle: The WITH clause recursive substatement
description: This section specifies the syntax and semantics of the WITH clause recursive substatement
menu:
  latest:
    identifier: recursive-with
    parent: with-clause
    weight: 30
isTocNested: true
showAsideToc: true
---

The optional `RECURSIVE` keyword fundamentally changes the meaning of a `WITH` clause substatement. The recursive variant lets you implement SQL solutions that, without it, at best require verbose formulations involving, for example, self-joins. In the limit, the `WITH` clause recursive substatement lets you implement SQL solutions that otherwise would require procedural programming.

## Syntax

When the optional `RECURSIVE` keyword is used, the [`with_clause_substatement_defn`](../../../syntax_resources/grammar_diagrams/#with-clause-substatement-defn) must be a `SELECT` statement—and this must have a specific form as the `UNION` or `UNION ALL` of the so-called _non-recursive term_ and the _recursive term_, thus:

```
with
  recursive <name>(c1, c2, ...) as (

    -- Non-recursive term.
    (
      select ...
    )

    union [all]

    -- Recursive term (notice the recursive self-reference to <name>.
    (
      select ... from <name> ...
    )
  )
select ... from <name> ...;
```
The following minimal example is found in very many articles and in the documentation for several SQL databases.

```plpgsql
with
  recursive r(n) as (

-- Non-recursive term.
    (
      values(1)
    )

    union all

    -- Recursive term.
    (
      select n + 1
      from r
      where n < 5
    )
  )
select n from r order by n;
```
Notice that the parentheses that surround the _non-recursive term_ and the _recursive term_ are not required. They are used for clarity. See the section [The _recursive term_ must be parenthesised to allow this to use a WITH clause](#the-recursive-term-must-be-parenthesised-to-allow-this-to-use-a-with-clause) for a scenario where the parentheses _are_ essential.

This is the result:

```
 n 
---
 1
 2
 3
 4
 5
```

The [Semantics](#semantics) section explains how a `WITH` recursive substatement is evaluated. When you understand this, you can predict the result of this minimal example and, by induction, the result of any arbitrarily complex example.

## Restrictions

### Maximum one WITH clause recursive substatement

The attempt to define more than one recursive substatement within a particular `WITH` clause causes a generic _42601_ syntax error. You can work around this restriction by pushing it down by one level of nesting, thus:

```plpgsql
with
  a1(n) as (
    select 42),

  a2(n) as (
    with
      recursive r(n) as (
        values(1)

        union all

        select n + 1
        from r
        where n < 5
        )
    select n from r),

  a3(n) as (
    select 99)

(
  select n from a1
  union all
  select n from a2
  union all
  select n from a3
)
order by n desc;
```

This is the result:

```
 n  
----
 99
 42
  5
  4
  3
  2
  1
```

### The WITH clause recursive substatement must be first in the clause

This code:

```plpgsql
with
  a1(n) as (
    select 42),

  recursive r(n) as (
    values(1)

    union all

    select n + 1
    from r
    where n < 5
    ),

  a2(n) as (
    select 99)

(
  select n from r
  union all
  select n from a2
)
order by n desc;
```

causes a _42601_ syntax error to be reported for the line `recursive r(n) as...` If you remove this:

```
a1(n) as (
    select 42),
```

then the statement executes without error to produce this result:

```
 n  
----
 99
  5
  4
  3
  2
  1
```

Alternatively, you can push down the `WITH` clause recursive substatement one level as shown above.

### The recursive term must be parenthesised to allow this to use a WITH clause

First try this positive example:

```plpgsql
with
  recursive r(n) as (
    (
      with a1(n) as (
        values(1))
      select n from a1
    )

    union all

    (
      with a2(n) as (
        select n + 1
        from r
        where n < 5)
      select n from a2
    )
  )
select n from r order by n;
```

Notice that this is simply a _syntax_ example. Using `WITH` clauses within the recursive and non-recursive terms brings no value. The statement executes without error and produces this result:

```
 n 
---
 1
 2
 3
 4
 5
```

Next, first, remove the parenthesis pair that surrounds the _non-recursive term_. The statement runs without error to produce the same result. Now, re-instate this pair and remove the parenthesis pair that surrounds the _recursive term_. You get the generic _42601_ syntax error, reported for this line:

```
with a2(n) as (
```

### Aggregate functions are not allowed in a recursive term

Try this:

```plpgsql
with
  recursive r(n) as (
    (
      values(1)
    )

    union all

    (
      select max(n) + 1
      from r
      where n < 5
    )
  )
select n from r order by n;
```

It causes this specific error:

```
42P19: aggregate functions are not allowed in a recursive query's recursive term
```
The restriction, more carefully stated, disallows aggregate functions when the `FROM` list item is the recursive self-reference. By extension, the keywords `GROUP BY` and `HAVING` are also disallowed when the `FROM` list item is the recursive self-reference.

Here is an example that executes without error that uses an aggregate function on a `FROM` list item that is _not_ the recursive self-reference:

```plpgsql
set client_min_messages = warning;
drop table if exists t cascade;
create table t(n int primary key);
insert into t(n) values (1), (2), (3);

with
  recursive r(n) as (
    (
      values(1)
    )

    union all

    (
      select n + (select min(n) from t)
      from r
      where n < 5
    )
  )
select n from r order by n;
```

This is the results:

```
 n 
---
 1
 2
 3
 4
 5
```

## Semantics

You can find various formulations of the following explanation by Internet search. In particular, here is a version in the [PostgreSQL Version 11 documentation](https://www.postgresql.org/docs/11/queries-with.html#id-1.5.6.12.5.4).

In informal prose:

- The _non-recursive term_ is invoked just once and establishes a starting relation.
- The _recursive term_ is invoked time and again. On its first invocation, it acts on the relation produced by the evaluation of the _non-recursive term_. On subsequent invocations, it acts on the relation produced by its previous invocation.
- Each successive term evaluation appends its output to the growing result of the recursive substatement.
- The repeating invocation of the _recursive term_ stops when it produces an empty relation.

### Pseudocode definition of the semantics

A compact, and exact, formulation is given by using pseudocode. The _"RECURSIVE_WITH_RESULTS"_ table, the _"WORKING_RESULTS"_ table, and the _"TEMP_RESULTS"_ table are transient, statement-duration, structures.

> - **Purge the RECURSIVE_WITH_RESULTS table and the WORKING_RESULTS table.**
>
> - **Evaluate the non-recursive term, inserting the resulting rows into the WORKING_RESULTS table.**
>
> - **Insert the contents of the WORKING_RESULTS table into the RECURSIVE_WITH_RESULTS table.**
>
> - **while the WORKING_RESULTS table is not empty loop**
>
>   - **Purge the TEMP_RESULTS table.**
>   
>   - **Evaluate the recursive term using the current contents of the WORKING_RESULTS table for the recursive self-reference, inserting the resulting rows into the TEMP_RESULTS table.**
>   
>   - **Purge the WORKING_RESULTS table and insert the contents of the TEMP_RESULTS table.**
>   
>   - **Append the contents of the TEMP_RESULTS table into the RECURSIVE_WITH_RESULTS table.**
>   
>- **end loop**
> 
>- **Deliver the present contents of the RECURSIVE_WITH_RESULTS table as the final result.**

### PL/pgSQL procedure implementation of the pseudocode : example 1

This pseudocode can be easily implemented, as a PL/pgSQL procedure, for the minimal example shown in the [Syntax](#syntax) section above. First, do this set-up:

```plpgsql
set client_min_messages = warning;

drop procedure if exists recursive_with_semantics_1 cascade;
drop table if exists recursive_with_results cascade;
drop table if exists working_results cascade;
drop table if exists temp_results cascade;

create table recursive_with_results(n int primary key);
create table working_results(n int primary key);
create table temp_results(n int primary key);
```
Now create the procedure:

```plpgsql
create procedure recursive_with_semantics_1(max_n in int)
  language plpgsql
as $body$
begin
  -- Emulate the non-recursive term.
  delete from recursive_with_results;
  delete from working_results;
  insert into working_results(n) values(1);
  insert into recursive_with_results(n) select n from working_results;

  -- Emulate the recursive term.
  while ((select count(*) from working_results) > 0) loop
    delete from temp_results;
    insert into temp_results
    select n + 1 from working_results
    where n < max_n;

    delete from working_results;
    insert into working_results(n) select n from temp_results;
    insert into recursive_with_results(n) select n from temp_results;
  end loop;
end;
$body$;
```

Notice that the [PostgreSQL Version 11 documentation](https://www.postgresql.org/docs/11/queries-with.html#id-1.5.6.12.5.4) says this:

> Strictly speaking, this process is iteration not recursion, but RECURSIVE is the terminology chosen by the SQL standards committee.

This is a somewhat dubious claim because, in any language, recursion is implemented at a lower level in the hierarchy of abstractions, as iteration. The code of the PL/pgSQL procedure, because it uses a `WHILE` loop, makes this point explicitly.

Now invoke the procedure and observe the contents of the _"RECURSIVE_WITH_RESULTS"_ table:

```plpgsql
call recursive_with_semantics_1(5);
select n from recursive_with_results order by 1;
```

The result is identical to that produced by the SQL implementation that it emulates (shown in the [Syntax](#syntax) section above).

### PL/pgSQL procedure implementation of the pseudocode : example 2

Try this extended version of the minimal example:

```plpgsql
with
  recursive r(c1, c2) as (

    -- Non-recursive term.
    (
      values (0, 1), (0, 2), (0, 3)
    )

    union all

    -- Recursive term.
    (
      select c1 + 1, c2 + 1
      from r
      where c1 < 4
    )
  )
select c1, c2 from r order by c1, c2;
```
This is the result:

```
 c1 | c2 
----+----
  0 |  1
  0 |  2
  0 |  3
  
  1 |  2
  1 |  3
  1 |  4
  
  2 |  3
  2 |  4
  2 |  5
  
  3 |  4
  3 |  5
  3 |  6
  
  4 |  5
  4 |  6
  4 |  7
```

The whitespace was added by hand to group the results into those produced first by evaluating the _non-recursive term_ and then those produced by each successive repeat evaluation of the _recursive term_.

The procedural implementation that emulates the pseudocode is a natural extension of the [first example](#pl-pgsql-procedure-implementation-of-the-pseudocode-example-1). First, do this set-up:

```plpgsql
set client_min_messages = warning;

drop procedure if exists recursive_with_semantics_2 cascade;
drop table if exists recursive_with_results cascade;
drop table if exists working_results cascade;
drop table if exists temp_results cascade;

create table recursive_with_results(
  c1 int not null,
  c2 int not null,
  constraint recursive_with_results_pk primary key(c1, c2));

create table working_results(
  c1 int not null,
  c2 int not null,
  constraint working_results_pk primary key(c1, c2));

create table temp_results(
  c1 int not null,
  c2 int not null,
  constraint temp_results_pk primary key(c1, c2));
```
Now create the procedure:

```plggsql
create procedure recursive_with_semantics_2(max_c1 in int)
  language plpgsql
as $body$
begin
  -- Emulate the non-recursive term.
  delete from recursive_with_results;
  delete from working_results;
  insert into working_results(c1, c2) values (0, 1), (0, 2), (0, 3);
  insert into recursive_with_results(c1, c2) select c1, c2 from working_results;

  -- Emulate the recursive term.
  while ((select count(*) from working_results) > 0) loop
    delete from temp_results;
    insert into temp_results
    select c1 + 1, c2 + 1 from working_results
    where c1 < max_c1;

    delete from working_results;
    insert into working_results(c1, c2) select c1, c2 from temp_results;
    insert into recursive_with_results(c1, c2) select c1, c2 from temp_results;
  end loop;
end;
$body$;
```

Notice that, here, each iteration accumulates _three_ rows.

Now invoke the procedure and observe the contents of the _"RECURSIVE_WITH_RESULTS"_ table:

```plpgsql
call recursive_with_semantics_2(4);
select c1, c2 from recursive_with_results order by c1, c2;
```

The result is identical to that produced by the SQL implementation that it emulates.

The section [Using a WITH clause recursive substatement to traverse graphs of all kinds](../traversing-general-graphs/) shows how to do graph traversal of undirected and directed graphs using application-agnostic examples. When the graph is cyclic, it shows how to detect and prevent endless repetition.

## Case studies

The following two sections describe how to use a `WITH` clause recursive substatement to implement two practical cases:

- [Case study—using a WITH clause recursive substatement to traverse a hierarchy](../emps-hierarchy) describes the use case (traversing an employee hierarchy) that is most commonly used to illustrate a practical application of the `WITH` clause recursive substatement. Different SQL databases with different variants of SQL use importantly different approaches. PostgreSQL, and therefore YSQL, have only standard SQL features here (and not, therefore, the `CONNECT BY PRIOR` feature that is typically used with Oracle Database). Neither do they have dedicated syntax to ask for breadth-first or depth-first traversal. Rather, these two kinds of traversal must be programmed explicitly. The explicit solutions use array functionality and are straightforward. Moreover, using this approach allows various second-order display choices easily to be implemented.
- [Using a `WITH` clause recursive substatement to traverse graphs of all kinds](../traversing-general-graphs/) leading to [Computing Bacon Numbers for actors listed in the IMDb](../bacon-numbers/). The approach to traversing graphs of all kinds is a natural extension of the approach shown for the employee hierarchy traversal. It adds logic to accommodate the fact that the edges are undirected and for cycle prevention. However, this straightforward approach collapses when, as is the case with the IMBd data, there are very many paths between most pairs of actors. This brings an exponential explosion in both time to completion and use of memory. The Bacon Numbers account shows how to avoid this collapse by implementing the algorithm that the `WITH` clause recursive substatement implements using explicit SQL. This allows early pruning to leave only the shortest paths with each repetition of the _recursive term_.



