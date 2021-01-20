---
title: Case study—using a WITH clause recursive substatement to traverse an employee hierarchy 
linkTitle: case study—traversing an employee hierarchy
headerTitle: Case study—traversing an employee a hierarchy
description: Case study to show how to traverse a hierarchy, breadth or depth first, using a WITH clause recursive substatement.
menu:
  latest:
    identifier: emps-hierarchy
    parent: with-clause
    weight: 40
isTocNested: true
showAsideToc: true
---



A hierarchy is a specialization of the general notion of a graph—and, as such, it's the simplest kind of graph that still deserves that name. The taxonomy of successive specializations starts with the most general (the _undirected cyclic graph_) and successively descends to the most restricted, a hierarchy. The taxonomy refers to a hierarchy as a _rooted tree_. All this explained in the section [Using a WITH clause recursive substatement to traverse graphs of all kinds](..//traversing-general-graphs/). 

The representation of a general graph requires an explicit, distinct, representation of the nodes and the edges. Of course, a hierarchy can be represented in this way. But because of how it's restricted, it allows a simpler representation in a SQL database where only the nodes are explicitly represented, in a single table, and where the edges are inferred using a self-referential foreign key. A _"parent ID"_ column (list) references the table's primary key—the _"ID"_ column (list). This is  enforced by a foreign key constraint. (This is referred to as a one-to-many recursive relationship, or one-to-many "pig's ear", in the jargon of entity-relationship modeling.) The ultimate, unique, root of the hierarchy has the _"parent ID"_ set to `NULL`.

The SQL that this section presents uses the simpler representation. But the section  [Using a WITH clause recursive substatement to traverse graphs of all kinds](..//traversing-general-graphs/) shows, for completeness, that the general SQL that you need to follow edges in a general graph works when the general representation happens to describe a hierarchy.

{{< tip title="Download a zip of scripts that include all the code examples that implement this case study" >}}

All of the `.sql` scripts that this case study presents for copy-and-paste at the `ysqlsh` prompt are included for download in a zip-file.

[Download `recursive-with-case-studies.zip`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/recursive-with-case-studies/recursive-with-case-studies.zip).

After unzipping it on a convenient new directory, you'll see a `README.txt`.  It tells you how to start the master-script. Simply start it in `ysqlsh`. You can run it time and again. It always finishes silently. You can see the report that it produces on a dedicated spool directory and confirm that your report is identical to the reference copy that is delivered in the zip-file.
{{< /tip >}}

## Create and populate the "emps" table

The following code creates a table of employees that records each employee's manager this way. This is a stylized example where _"name"_ serves as the primary key column, _"mgr_name"_ serves as the foreign key column, and there are no other columns.

##### `cr-table.sql`

```plpgsql
set client_min_messages = warning;
drop table if exists emps cascade;
create table emps(
  name     text primary key,
  mgr_name text);

-- The order of insertion is arbitrary
insert into emps(name, mgr_name) values
  ('mary',   null),
  ('fred',  'mary'),
  ('susan', 'mary'),
  ('john',  'mary'),
  ('doris', 'fred'),
  ('alice', 'john'),
  ('bill',  'john'),
  ('joan',  'bill'),
  ('george', 'mary'),
  ('edgar', 'john'),
  ('alfie', 'fred'),
  ('dick',  'fred');

-- Implement the one-to-many "pig's ear".
alter table emps
add constraint emps_mgr_name_fk
foreign key(mgr_name) references emps(name)
on delete restrict;

-- The ultimate manager has no manager.
-- Enforce the business rule "Maximum one ultimate manager".
create unique index t_mgr_name on emps((mgr_name is null)) where mgr_name is null;
```

Stress the constraints with this attempt to insert a second ultimate manager:

```plpgsql
insert into emps(name, mgr_name) values ('bad', null);
```

It cases this error:

```
23505: duplicate key value violates unique constraint "t_mgr_name"
```

Stress the constraints with this attempt to delete an employee with reports:

```plpgsql
delete from emps where name = 'fred';
```

It cases this error:

```
23503: update or delete on table "emps" violates foreign key constraint "emps_mgr_name_fk" on table "emps"
Key (name)=(fred) is still referenced from table "emps".
```

This constraint, together with the fact that the "_mgr_name"_ column is nullable, guarantees the invariant that there is exactly one ultimate manager—in other words that the employee graph is a tree (a.k.a. hierarchy). Check it thus:

##### `do-assert-is-hierarchy.sql`

```plpgsql
do $body$
begin
  assert (select count(*) from emps where mgr_name is null) = 1,
    'Rule "Employee graph is a hierarchy" does not hold';
end;
$body$;
```

## List the employees top-down with their immediate managers in breadth first order

This simplest formulation of the query to list the employees with their immediate managers uses a `WITH` clause that has a recursive substatement like this:

##### `cr-view-top-down-simple.sql`

```plpgsql
create or replace view top_down_simple(depth, mgr_name, name) as
with
  recursive hierarchy_of_emps(depth, mgr_name, name) as (

    -- Non-recursive term.
    -- Select the exactly one ultimate manager.
    -- Define this emp to be at depth 1.
    (
      select
        1,
        '-',
        name
      from emps             
      where mgr_name is null
    )

    union all

    -- Recursive term.
    -- Treat the emps from the previous iteration as managers.
    -- Join these with their reports, if they have any.
    -- Increase the emergent depth by 1 with each step.
    -- Stop when none of the current putative managers has a report.
    -- Each successive iteration goes one level deeper in the hierarchy.
    (
      select
        h.depth + 1,
        e.mgr_name,
        e.name
      from
      emps as e
      inner join
      hierarchy_of_emps as h on e.mgr_name = h.name 
    )
  )
select
  depth,
  mgr_name,
  name
from hierarchy_of_emps;
```

Each successive iteration of the _recursive term_ accumulates the set of direct reports, where such a report exists, of the employees produced first by the _non-recursive term_ (i.e. the ultimate manager) and then by the employees produced by the previous  iteration of the _recursive term_. The iteration stops when none of the employees produced by  the previous  iteration of the _recursive term_ has a report. 

Notice that the choice to define the ultimate manager to be at depth _1_ is just that: a design choice. You might prefer to define the ultimate manager to be at depth _0_, arguing that it's better to interpret this measure as the number of managers up the chain that the present employee has.

The result set that this view represents is usually ordered first by the calculated _"depth"_ column and then by usefully orderable actual columns—the manager name and then the employee name in the case of the "_emps"_ example table:

##### `do-breadth-first-query.sql`

```plpgsql
select
  depth,
  mgr_name,
  name
from top_down_simple
order by
  depth,
  mgr_name nulls first,
  name;
```

This produces a so-called "breadth first" order. This is the result:

```
 depth | mgr_name |  name  
-------+----------+--------
     1 | -        | mary
     2 | mary     | fred
     2 | mary     | george
     2 | mary     | john
     2 | mary     | susan
     3 | fred     | alfie
     3 | fred     | dick
     3 | john     | alice
     3 | john     | bill
     3 | john     | edgar
     4 | bill     | joan
```

## List the path top-down from the ultimate manager to each employee in breadth first order

The term of art "path" denotes the list of managers from the ultimate manager through each next direct report down to the current employee. It is easily calculated by using array concatenation as described in the [The&nbsp;||&nbsp;operator](../../../datatypes/type_array/functions-operators/concatenation/#the-160-160-160-160-operator) subsection of the [Array data types and functionality](../../../datatypes/type_array/) major section.

##### `cr-view-top-down-path.sql`

```plpgsql
create or replace view top_down_path(path) as
with
  recursive hierarchy_of_emps(path, name) as (
    (
      select array['n/a'], name
      from emps
      where mgr_name is null
    )

    union all

    (
      select h.path||e.mgr_name, e.name
      from emps as e
      inner join hierarchy_of_emps as h on e.mgr_name = h.name 
    )
  )
select path[2:cardinality(path)]||name as path
from hierarchy_of_emps;
```

The cardinality of the path represents the depth. The result set that this view represents can be easily ordered first by the emergent _"depth"_ and then by the employee name:

##### `do-breadth-first-path-query.sql`

```plpgsql
select cardinality(path) as depth, path
from top_down_path
order by
  depth,
  path[cardinality(path)];
```

This is the result:

```
 depth |         path          
-------+-----------------------
     1 | {mary}
     2 | {mary,fred}
     2 | {mary,george}
     2 | {mary,john}
     2 | {mary,susan}
     3 | {mary,fred,alfie}
     3 | {mary,john,alice}
     3 | {mary,john,bill}
     3 | {mary,fred,dick}
     3 | {mary,john,edgar}
     4 | {mary,john,bill,joan}
```

Notice that the _"top_down_path"_ view has the same information content as the _"top_down_simple"_ view. But it's easier to read because you don't need mentally to construct the path by looking, recursively, for the row that has the "_mgr_name"_ of the row of interest as its _"name"_ until you reach the ultimate manager.

This `assert` confirms the conclusion:

##### `do-assert-top-down-path-view-and-top-down-simple-view-same-info.sql`

```plpgsql
do $body$
declare
  c constant int not null :=
    (
      with
        a1(depth, mgr_name, name) as (
          select depth, mgr_name, name from top_down_simple),

        a2(depth, mgr_name, name) as (
          select
            cardinality(path),
            case cardinality(path)
              when 1 then '-'
              else        path[cardinality(path) - 1]
            end,
            path[cardinality(path)]
          from top_down_path),

        a1_except_a2(depth, mgr_name, name) as (
          select depth, mgr_name, name from a1
          except
          select depth, mgr_name, name from a2),

        a2_except_a1(depth, mgr_name, name) as (
          select depth, mgr_name, name from a2
          except
          select depth, mgr_name, name from a1)

      select count(*) from
      (
        select depth, mgr_name, name from a1_except_a2
        union all
        select depth, mgr_name, name from a2_except_a1
      ) as n
    );
begin
 assert c = 0, 'Unexpected';
end;
$body$;
```

This "`UNION ALL` of two complementary `EXCEPT` queries" is the standard pattern for checking if two different relations have the same content. Notice how the use of a `WITH` clause with ordinary (non-recursive) substatements lets you express the logic in a maximally readable way.

## List the path top-down from the ultimate manager to each employee in depth-first order

Do this:

##### `do-depth-first-query.sql`

```plpgsql
select cardinality(path) as depth, path
from top_down_path
order by
  path[2] asc nulls first,
  path[3] asc nulls first,
  path[4] asc nulls first,
  path[5] asc nulls first;
```

This is the result:

```
 depth |         path          
-------+-----------------------
     1 | {mary}
     2 | {mary,fred}
     3 | {mary,fred,alfie}
     3 | {mary,fred,dick}
     2 | {mary,george}
     2 | {mary,john}
     3 | {mary,john,alice}
     3 | {mary,john,bill}
     4 | {mary,john,bill,joan}
     3 | {mary,john,edgar}
     2 | {mary,susan}
```

You can see that the results are sorted in depth-first order with the employees at the same depth ordered by _"name_". The design of the `ORDER BY` clause relies on the following fact—documented in the [Array data types and functionality](../../../datatypes/type_array/#synopsis) major section:

> If you read a within-array value with a tuple of index values that put it outside of the array bounds, then you silently get `NULL`.

Notice that this:

```plpgsql
select max(cardinality(path)) from top_down_path;
```
returns the value _4_ for the present example. This means that `path[5]` returns `NULL`—as would, for example, `path[6]`, `path[17]`, and `path[42]`. When such a query is issued programmatically, you can determine the maximum path cardinality and build the `ORDER BY	` clause to have just the necessary and sufficient number of terms. Alternatively, for simpler code, you could write it with a number of terms that exceeds your best estimate of the maximum cardinality of the arrays that your program will have to deal with, ensuring safety with a straightforward test of the actual maximum cardinality.

## Pretty-printing the top-down depth-first report of paths

Users often like to use indentation to visualize hierarchical depth. This is easy to achieve, thus:

##### `do-indented-depth-first-query.sql`

```plpgsl
select
  rpad(' ', 2*cardinality(path) - 2, ' ')||path[cardinality(path)] as "emps hierarchy"
from top_down_path
order by
  path[2] asc nulls first,
  path[3] asc nulls first,
  path[4] asc nulls first,
  path[5] asc nulls first;
```

This is the result:

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

You've probably seen how the Unix `tree` command presents the hierarchy that it computes using the long vertical bar, the long horizontal bar, the sideways "T", and the "L" symbols: `│`, `─`, `├`, and `└`. It's easy to output an approximation to this, that omits the long vertical bar, with a single SQL statement. The trick is to use the [`lead()`](../../../exprs/window_functions/function-syntax-semantics/lag-lead/#lead) window function to calculate the _"next_depth"_ for each row as well as the _"depth"_. If the _"next_depth"_ value is equal to the _"depth"_ value: then output the sideways "T"; else output the "L" (at the appropriate indentation level).

##### `do-unix-tree-query.sql`

```plpgsql
with
  a1 as (
    select
      cardinality(path) as depth,
      path
    from top_down_path),

  a2 as (
    select
      depth,
      lead(depth, 1, 0) over w as next_depth,
      path
    from a1
    window w as (
      order by
        path[2] asc nulls first,
        path[3] asc nulls first,
        path[4] asc nulls first,
        path[5] asc nulls first))

select
  case depth = next_depth
    when true then
      lpad(' ├── ', (depth - 1)*5, ' ')
    else
      lpad(' └── ', (depth - 1)*5, ' ')
  end
  ||
  path[depth] as "Approx. 'Unix tree'"
from a2
order by
  path[2] asc nulls first,
  path[3] asc nulls first,
  path[4] asc nulls first,
  path[5] asc nulls first;
```

Using _0_ as the actual for the third (optional) `lag()` parameter means that the function will return _0_ for the last row in the sorted order rather than `NULL`. This allows a simple equality test to be used reliably.

This is the result:

```
 Approx. 'Unix tree' 
---------------------
 mary
  └── fred
       ├── alfie
       └── dick
  ├── george
  └── john
       ├── alice
       └── bill
            └── joan
       └── edgar
  └── susan
```

It's easy to transform this result _manually_ into the format that Unix uses by filling in vertical bars as appropriate, thus:

```
 mary
  ├── fred
  │    ├── alfie
  │    └── dick
  ├── george
  ├── john
  │    ├── alice
  │    ├── bill
  │    │    └── joan
  │    └── edgar
  └── susan
```

Of course, when you do this, you're intuitively following a rule. So it could certainly be implemented in procedural code, using this harness:

##### `cr-function-unix-tree.sql`

```plpgsql
drop function if exists unix_tree() cascade;

create function unix_tree()
  returns table(t text)
  language plpgsql
as $body$
<<b>>declare
  results text[] not null := '{}';
begin
  with
    a1 as (
      select
        cardinality(path) as depth,
        path
      from top_down_path),

    a2 as (
      select
        depth,
        lead(depth, 1, 99) over w as next_depth,
        path
      from a1
      window w as (
        order by
          path[2] asc nulls first,
          path[3] asc nulls first,
          path[4] asc nulls first,
          path[5] asc nulls first)),

    results as (
      select
        case depth = next_depth
          when true then
            lpad(' ├── ', (depth - 1)*5, ' ')
          else
            lpad(' └── ', (depth - 1)*5, ' ')
        end
        ||
        path[depth] as result
      from a2
      order by
        path[2] asc nulls first,
        path[3] asc nulls first,
        path[4] asc nulls first,
        path[5] asc nulls first)

  select array_agg(r.result)
  into b.results
  from results as r;

  foreach t in array results loop
    -- Implement the logic to transform the present results
    -- into the Unix "tree" format by adding vertical bars
    -- as approapriate.
  end loop;

  foreach t in array results loop
    return next;
  end loop;
end b;
$body$;

select t from unix_tree();
```

This is a sketch of the required logic, expressed crudely and somewhat approximately:

- Scan each result row looking for an "L" in any of the character positions that it might occur.
- When an "L" is found, look forward over as many result rows as it takes until you find the first non-space character in the character position where the "L" was found.
  - If this is found on the immediately next row, then do nothing; move to the next row, and start again.
  - Otherwise, and when the next non-space character is an "L" or a "T", substitute a "T" for the starting "L" or "T" and substitute a vertical bar for the remaining spaces. When the next non-space is not an "L" or "T", leave the spaces as is.
- Repeat this for each character position where an "L" is found.

Implementing, and testing, the logic is left as an exercise for the reader.

## List the path bottom-up from a chosen employee to the ultimate manager

The essential property of a hierarchy is that each successive upwards step defines exactly one parent (in this example, exactly one manager) by traversing the foreign key reference in the "many" to "one" direction. It's this property that distinguishes a hierarchy from a more general graph. This means that there is no distinction between breadth-first and depth-first when traversing a hierarchy upwards.

Here is the query. It's presented using the prepare-and-execute paradigm so that you can supply the starting employee of interest as a parameter. Notice that _"path"_ is not yet defined in the _non-recursive term_. This means that the only practical design for the _"depth"_ notion here is different from what was used in the top-down approach: the design chosen has it starting at _0_ and _increasing_ by _1_ with each step up the hierarchy.

##### `do-bottom-up-simple-query.sql`

```plpgsql
deallocate all;

prepare bottom_up_simple(text) as
with
  recursive hierarchy_of_emps(depth, name, mgr_name) as (

    -- Non-recursive term.
    -- Select the exactly one employee of interest.
    -- Define the depth to be zero.
    (
      select
        0,
        name,
        mgr_name
      from emps             
      where name = $1
    )

    union all

    -- Recursive term.
    -- Treat the emps from the previous iteration as reports.
    -- Join these with their managers.
    -- Increase the depth with each step upwards.
    -- Stop when the current putative report has no manager, i.e. is
    -- the ultimate manager.
    -- Each successive iteration goes one level higher in the hierarchy.
    (
      select
        h.depth + 1,
        e.name,
        e.mgr_name
      from
      emps as e
      inner join
      hierarchy_of_emps as h on h.mgr_name = e.name
    )
  )
select
  depth,
  name,
  coalesce(mgr_name, null, '-') as mgr_name
from hierarchy_of_emps;

execute bottom_up_simple('joan');
```

This is the result:

```
 depth | name | mgr_name 
-------+------+----------
     0 | joan | bill
     1 | bill | john
     2 | john | mary
     3 | mary | -
```

Alternatively, you could encapsulate the query in a function to deliver the same benefit. In this scheme, the result is a single array that represents the path from bottom to top.

##### `cr-function-bottom-up-path.sql`

```plpgsql
drop function if exists bottom_up_path(text) cascade;

create function bottom_up_path(start_name in text)
  returns text[]
  language sql
as $body$
  with
    recursive hierarchy_of_emps(mgr_name, path) as (
      (
        select mgr_name, array[name]
        from emps             
        where name = start_name
      )
      union all
      (
        select e.mgr_name, h.path||e.name
        from
        emps as e
        inner join
        hierarchy_of_emps as h on e.name = h.mgr_name
      )
    )
  select path
  from hierarchy_of_emps
  order by cardinality(path) desc
  limit 1;
$body$;

select bottom_up_path('joan');
```

This is the result:

```
    bottom_up_path     
-----------------------
 {joan,bill,john,mary}
```

You can produce a prettier output like this:

##### `cr-function-bottom-up-path-display.sql`

```plpgsql
drop function if exists bottom_up_path_display(text) cascade;
create function bottom_up_path_display(start_name in text)
  returns text
  language plpgsql
as $body$
declare
  path constant text[] not null := (bottom_up_path(start_name));
  t text not null := path[1];
begin
  for j in 2..cardinality(path) loop
    t := t||' > '||path[j];
  end loop;
  return t;
end;
$body$;

select bottom_up_path_display('joan');
```

This is the result:

```
  bottom_up_path_display   
---------------------------
 joan > bill > john > mary
```
