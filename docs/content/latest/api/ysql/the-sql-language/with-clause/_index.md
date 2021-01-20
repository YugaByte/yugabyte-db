---
title: The WITH clause [YSQL]
headerTitle: The WITH clause (Common Table Expression)
linkTitle: WITH clause
description: How to use the WITH clause a.k.a. Common Table Expression
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: with-clause
    parent: the-sql-language
    weight: 200
isTocNested: true
showAsideToc: true
---

The `WITH` clause can be used as part of a `SELECT` statement, an `INSERT` statement, an `UPDATE` statement, or a `DELETE` statement. For this reason, the functionality is described in this dedicated section.

{{< tip title="Download a zip of WITH clause demonstration scripts" >}}

The [`recursive-with-case-studies.zip`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/recursive-with-case-studies/recursive-with-case-studies.zip) file contains all the `.sql` scripts that illustrate these sections:

- [Case study—using a WITH clause recursive substatement to traverse a hierarchy](./emps-hierarchy/)

- [Using a WITH clause recursive substatement to traverse graphs of all kinds](./traversing-general-graphs/)

- [Case study—computing Bacon Numbers for actors listed in the IMDb](./bacon-numbers/).

After unzipping it on a convenient new directory, you'll see a `README.txt`.  It tells you how to start, in turn, a few master-scripts. Simply start each in `ysqlsh`. You can run these time and again. Each one always finishes silently. You can see the reports that they produce on the dedicated spool directories and confirm that the files that are spooled are identical to the corresponding reference copies that are delivered in the zip-file.
{{< /tip >}}

## Introduction

The `WITH` clause lets you name one or several SQL substatements. Such a substatement may be any of these kinds: `SELECT`, `VALUES`, `INSERT`, `UPDATE`, or `DELETE`. And the `WITH` clause is legal at the start of any of these kinds of statement : `SELECT`, `INSERT`, `UPDATE`, or `DELETE`. (`VALUES` is missing from the second list.) There are two kinds of `WITH` clause substatement: the _ordinary_ kind; and the _recursive_ kind.

The statement text that the ordinary kind of `WITH` clause substatement names has the same syntax as the a SQL statement that you issue at top level. However, the recursive kind of substatement may be used _only_ in a `WITH` clause. Each such `WITH` clause component (the name, an optional parenthesized column list) and the substatement itself) is sometimes known as a _common table expression_.

A `WITH` clause can, for example, be used to provide values for, say, an `INSERT` like this:

```plpgsql
set client_min_messages = warning;
drop table if exists t1 cascade;
create table t1(k int primary key, v int not null);

with a(k, v) as (
  select g.v, g.v*2 from generate_series(11, 20) as g(v)
  )
insert into t1
select k, v from a;

select k, v from t1 order by k;
```

This component:

```
a(k, v) as (select g.v, g.v*2 from generate_series(11, 20) as g(v))
```

is an example of a common table expression. The YSQL documentation avoids using the term _common table expression_

This is the result:

```
 k  | v  
----+----
 11 | 22
 12 | 24
 13 | 26
 14 | 28
 15 | 30
 16 | 32
 17 | 34
 18 | 36
 19 | 38
 20 | 40
```

Moreover, data-changing substatements (`INSERT`, `UPDATE` , and `DELETE`) can be used in a `WITH` clause and, when these use a `RETURNING` clause, the returned values can be used in another data-changing statement like this:

```plpgsql
set client_min_messages = warning;
drop table if exists t2 cascade;
create table t2(k int primary key, v int not null);

with moved_rows as (
  delete from t1
  where k > 15
  returning k, v)
insert into t2(k, v)
select k, v from moved_rows;

(
  select 't1' as table_name, k, v from t1
  union all
  select 't2' as table_name, k, v from t2
  )
order by table_name, k;
```

The central notion is that a `WITH` clause lets you name one or more substatements (which each may be either `SELECT`, `VALUES`, `INSERT`, `UPDATE`, or `DELETE`) so that you can then refer to such a substatement by name, either in a subsequent substatement in that `WITH` clause or in the overall statement's final, main, substatement (which may be either `SELECT`, `INSERT`, `UPDATE`, or `DELETE`—but _not_ `VALUES`). In this way, a `WITH` clause substatement is analogous, in the declarative programming domain of SQL, to a procedure or a function in the imperative programming domain of a procedural language.

Notice that a schema-level view achieves the same effect, for `SELECT` or `VALUES`, as does a substatement in a view with respect to the named substatement's use. However a schema-level view cannot name a data-changing substatement. In this way, a `WITH` clause substatement brings valuable unique functionality. It also brings the modular programming benefit of hiding names, and the implementations that they stand for, from scopes that have no interest in them.

Finally, the use of a _recursive_ substatement in a `WITH` clause enables graph analysis. For example, an _"employees"_ table often has a self-referential foreign key like _"manager_id"_ that points to the table's primary key, _"employee_id"_. `SELECT` statements that use a `WITH` clause recursive substatement allow the reporting structure to be presented in various ways. This result shows the reporting paths of employees, in an organization with a strict hierarchical reporting scheme, in depth-first order. See the section [Pretty-printing the top-down depth-first report of paths](./emps-hierarchy/#pretty-printing-the-top-down-depth-first-report-of-paths).

```
 emps hierarchy 
----------------
 mary
   fred
     alfie
     dick
   george
   john
     alice
     bill
       joan
     edgar
   susan
```

The remainder of this section has the following subsections:

- [`WITH` clause—SQL syntax and semantics](./with-clause-syntax-semantics/)

- [The `WITH` clause recursive substatement](./recursive-with/)

- [Case study—using a WITH clause recursive substatement to traverse a hierarchy](./emps-hierarchy/)

- [Case study—computing Bacon Numbers for actors listed in the IMDb](./bacon-numbers/)

{{< tip title="Performance considerations." >}}

A SQL statement that uses a `WITH` clause sometimes gets a worse execution plan than the semantically equivalent statement that _doesn’t_ use a `WITH` clause. The explanation is usually that a “push-down” optimization of a restriction or projection hasn’t penetrated into the `WITH` clause’s substatement. You can usually avoid this problem by manually pushing down what you’d hope would be done automatically into your spellings of the `WITH` clause’s substatements.

Anyway, the ordinary good practice principle holds even more here: always check the execution plans of the SQL statements that your application issues, on representative samples of data, before moving the code to the production environment.

{{< /tip >}}

