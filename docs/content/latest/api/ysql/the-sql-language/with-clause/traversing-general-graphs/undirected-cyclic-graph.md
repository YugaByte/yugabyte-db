---
title: Using a WITH clause recursive substatement to traverse a general undirected cyclic graph
headerTitle: Finding the paths in a general undirected cyclic graph
linkTitle: undirected cyclic graph
description: This section shows how to use a WITH clause recursive substatement to traverse a general undirected cyclic graph.
menu:
  latest:
    identifier: undirected cyclic graph
    parent: traversing-general-graphs
    weight: 30
isTocNested: true
showAsideToc: true
---

Before trying any of the code in this section, make sure that you have created the _"edges"_ table (see [`cr-edges.sql`](../graph-representation/#cr-edges-sql)) and installed all the code shown in the section [Common code for traversing all kinds of graph](../common-code/). 

## Graph traversal using the denormalized "edges" table design

Start by populating the _"edges"_ table with the data that represents the data that defines the graph shown in the section [Undirected cyclic graph](../../traversing-general-graphs/#undirected-cyclic-graph) using the scheme that represents each undirected edge twice—once in each direction.

```plpgsql
delete from edges;
alter table edges drop constraint if exists edges_chk cascade;
alter table edges add constraint edges_chk check(node_1 <> node_2);

insert into edges(node_1, node_2) values
  ('n1', 'n2'),
  ('n2', 'n3'),
  ('n2', 'n4'),
  ('n3', 'n5'),
  ('n4', 'n5'),
  ('n4', 'n6'),
  ('n5', 'n6');

-- Implement the denormalization.
insert into edges(node_1, node_2)
select node_2 as node_1, node_1 as node_2
from edges;
```

Recall the SQL for traversing an employee hierarchy (see the section [List the path top-down from the ultimate manager to each employee in breadth first order](../../emps-hierarchy/#list-the-path-top-down-from-the-ultimate-manager-to-each-employee-in-breadth-first-order)):

```
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
from hierarchy_of_emps
```

The first attempt at the SQL for traversing the present undirected cyclic graph is easily written down just as obvious paraphrase:

```plpgql
deallocate all;

prepare stmt(text) as
with
  recursive paths(path) as (
    select array[$1, node_2]
    from edges
    where
    node_1 = $1

    union all

    select p.path||e.node_2
    from edges e, paths p
    where
    e.node_1 = terminal(p.path)
    )
select path
from paths;
```

Now execute it:

```plpgsql
execute stmt ('n1');
```

Though the statement compiled without error, and seems to be executing, it will never stop. You must interrupt it with `ctrl-C`. The reason, of course, is that the traversal reached one of the cycles (either _"n2→n3→n5→n4→n2..."_ or _"n2→n4→n5→n3→n2..."_) and will go around it time and again indefinitely. Extra SQL is needed to preempt this problem.

Notice that the next node along the path that is so-far defined by the row that the `WHERE` predicate tests for accumulation into the growing results set under the present repetition of the _recursive term_, and that would begin the unending cycle, might be anywhere along the present path, according to the graph's topology. The [`ANY`](../../../../datatypes/type_array/functions-operators/any-all/) test comes to the rescue. Try this:

```plpgsql
select ('n2' = any (array['n1', 'n2', 'n3', 'n5', 'n4']::text[]))::text as "cycle starting";
```

This is the result:

```
 cycle starting 
----------------
 true
```

It's easy to add this predicate to the `WITH` clause recursive substatement (in its _recursive term_). It's convenient to encapsulate the cycle-proof SQL in a _"language plpgsql"_ procedure so that the identity of the node chosen to seed the traversal can be named usefully. It's also useful to record the result in a table to allow trying various _ad hoc_ analysis queries after the fact.

##### `cr-find-paths-with-nocycle-check.sql`

```plpgsql
drop procedure if exists find_paths(text) cascade;

create procedure find_paths(seed in text)
  language plpgsql
as $body$
begin
  -- See "cr-find-paths-with-pruning.sql". This index demonstrates that
  -- no more than one path has been found to any particular terminal node.
  drop index if exists raw_paths_terminal_unq cascade;
  commit;
  delete from raw_paths;

  with
    recursive paths(path) as (
      select array[seed, node_2]
      from edges
      where
      node_1 = seed

      union all

      select p.path||e.node_2
      from edges e, paths p
      where
      e.node_1 = terminal(p.path)
      and not e.node_2 = any(p.path) -- <<<<< Prevent cycles.
      )
  insert into raw_paths(path)
  select path
  from paths;
end;
$body$;
```

Invoke it and the use the _["list_paths()"](../common-code/#cr-list-paths-sql)_ table function show the result thus:

```plpgsql
call find_paths(seed => 'n1');
\t on
select t from list_paths('raw_paths');
\t off
```

This is the result:

```
 path #   cardinality   path
 ------   -----------   ----
      1             2   n1 > n2
      
      2             3   n1 > n2 > n3
      3             3   n1 > n2 > n4
      
      4             4   n1 > n2 > n3 > n5
      5             4   n1 > n2 > n4 > n5
      6             4   n1 > n2 > n4 > n6
      
      7             5   n1 > n2 > n3 > n5 > n4
      8             5   n1 > n2 > n3 > n5 > n6
      9             5   n1 > n2 > n4 > n5 > n3
     10             5   n1 > n2 > n4 > n5 > n6
     11             5   n1 > n2 > n4 > n6 > n5
     
     12             6   n1 > n2 > n3 > n5 > n4 > n6
     13             6   n1 > n2 > n3 > n5 > n6 > n4
     14             6   n1 > n2 > n4 > n6 > n5 > n3
```

The whitespace was added manually to separate the paths into groups of the same cardinality to improve readability. The index on the _"path"_ column confirms that each of the fourteen paths is unique. Here they are. The pictures are numbered with the value assigned by [`row_number()`](../../../../exprs/window_functions/function-syntax-semantics/row-number-rank-dense-rank/#row-number) (see the implementation of _["list_paths()"](../common-code/#cr-list-paths-sql)_) and the cardinality groups are distinguished by boxing in each group.

![undirected-cyclic-graph-all-paths](/images/api/ysql/the-sql-language/with-clause/traversing-general-graphs/undirected-cyclic-graph-all-paths.jpg)

### Restrict down to a single shortest path to each non-seed node

As explained in the section [Create a procedure to filter the paths from "raw_paths" to leave only a shortest path to each distinct terminal node](../common-code/#cr-restrict-to-shortest-paths-sql), the [Bacon Numbers](../bacon-numbers) problem requires only that (one of the) the shortest path(s) from one node to every other node is found. The following approach produces the required numbers _after the fact_ on the table of _all_ paths.

Use the _["restrict_to_shortest_paths()"](../common-code/#cr-restrict-to-shortest-paths-sql)_ procedure to derive these paths and then show them:

```plpgsql
call restrict_to_shortest_paths('raw_paths', 'shortest_paths');
\t on
select t from list_paths('shortest_paths');
\t off
```

**Note:** As will be shown in the section [Produce the sets of shortest paths starting from every node](#produce-the-sets-of-shortest-paths-starting-from-every-node), it is interesting to accumulate the sets of shortest paths from each of many nodes into the _"shortest_paths"_ table. This is why the choice to purge the table before calling _"restrict_to_shortest_paths()"_ is left to the user. In contrast, the _"find_paths()"_ procedure starts by deleting all rows that the target _"raw_paths"_ table might contain. This is a design choice. If it simply accumulated paths from each successive call (invoking each of these with a different actual value for the _"seed"_ formal parameter) there'd be no errors. But the logic of the _"restrict_to_shortest_paths()"_ procedure would need to be more elaborate than the version that's presented here that expects to find only paths all of which have the same starting note. This would have made the essential core logic harder to understand.

Here is the result:

```
 path #   cardinality   path
 ------   -----------   ----
      1             2   n1 > n2
      2             3   n1 > n2 > n3
      3             3   n1 > n2 > n4
      4             4   n1 > n2 > n3 > n5
      5             4   n1 > n2 > n4 > n6
```

![undirected-cyclic-graph-shortest-paths](/images/api/ysql/the-sql-language/with-clause/traversing-general-graphs/undirected-cyclic-graph-shortest-paths.jpg)

Pruning after the fact in this way is a useful pedagogic device in that it clearly shows what information is sufficient to solve the [Bacon Numbers](../bacon-numbers) problem. However, pairs of actor nodes that are defined by the real IMDb data have huge numbers of connecting paths with a wide range of lengths because so many movies share the same subsets of many actors. This means that  when the approach that this section has explained is run on this kind of data, it takes a very long time to run and consumes a huge amount of memory for the ever-growing representation of the eventual result set. (There is no mechanism for spilling to disk.) The outcome is _either_ that the query fails to finish in reasonable time _or_ that it simply crashes with an "out of memory" error.

The remedy is obvious: at each repeat of the _recursive term_, compare the new about-to-be-accumulated rows with the entire set of already-accumulated rows and discard all new rows whose end node is the same as that of an already-found shorter path. It turns out that the constructs available to the SQL programmer of a `WITH` clause recursive substatement don't support this early pruning and so an approach must be designed that does allow this. This is the focus of the section [How to implement early path pruning](#how-to-implement-early-path-pruning).

### Produce the sets of shortest paths starting from every node

Do this to produce, in turn, all of the paths starting from each remaining node (i.e. apart from _"n1"_) and then to accumulate the output of _"restrict_to_shortest_paths"_ into the _"shortest_paths"_ table:

```plpgsql
do $body$
declare
  node text not null := '';
begin
  for node in (
    select distinct node_1 from edges
    where node_1 <> 'n1')
  loop
    call find_paths(seed => node);
    call restrict_to_shortest_paths('raw_paths', 'shortest_paths', append=>true);
  end loop;
end;
$body$;
```

Now show the result:

```plpgsql
\t on
select t from list_paths('shortest_paths');
\t off
```

This is the result:

```
 path #   cardinality   path
 ------   -----------   ----
      1             2   n1 > n2
      2             3   n1 > n2 > n3
      3             3   n1 > n2 > n4
      4             4   n1 > n2 > n3 > n5
      5             4   n1 > n2 > n4 > n6
      
      6             2   n2 > n1
      7             2   n2 > n3
      8             2   n2 > n4
      9             3   n2 > n3 > n5
     10             3   n2 > n4 > n6
     
     11             2   n3 > n2
     12             2   n3 > n5
     13             3   n3 > n2 > n1
     14             3   n3 > n2 > n4
     15             3   n3 > n5 > n6
     
     16             2   n4 > n2
     17             2   n4 > n5
     18             2   n4 > n6
     19             3   n4 > n2 > n1
     20             3   n4 > n2 > n3
     
     21             2   n5 > n3
     22             2   n5 > n4
     23             2   n5 > n6
     24             3   n5 > n3 > n2
     25             4   n5 > n3 > n2 > n1
     
     26             2   n6 > n4
     27             2   n6 > n5
     28             3   n6 > n4 > n2
     29             3   n6 > n5 > n3
     30             4   n6 > n4 > n2 > n1
```

The whitespace was added manually to separate the paths into groups that start, in turn, from each different node to improve readability.

## Graph traversal using the normalized "edges" table design

Start by populating the _"edges"_ table with the data that represents the data that defines the graph shown in the section [Undirected cyclic graph](../../traversing-general-graphs/#undirected-cyclic-graph) (just as above) but this time using the scheme that represents each undirected edge only in the arbitrarily chosen direction of how the node names (or numeric surrogate keys) sort.

```plpgsql
delete from edges;
alter table edges drop constraint if exists edges_chk cascade;
alter table edges add constraint edges_chk check(node_1 < node_2);

insert into edges(node_1, node_2) values
  ('n1', 'n2'),
  ('n2', 'n3'),
  ('n2', 'n4'),
  ('n3', 'n5'),
  ('n4', 'n5'),
  ('n4', 'n6'),
  ('n5', 'n6');
```
The choice of the definition of the constraint, _"node_1 < node_2"_ is arbitrary; _"node_2 < node_1"_ would work just as well as long as rows are populated appropriately. The inequality ensures _both_ that the same node isn't recorded in both columns _and_ that when a particular edge is recorded as, say, _('a', 'b')_, it cannot also be recorded in the other direction as _('b', 'a')_.

The `WHERE` clause predicate in the _non-recursive term_ that, in the SQL above, is written simply as `node_1 = seed`, must now be written thus:

```
node_1 = seed or node_2 = seed
```

And the `WHERE` clause predicate in the _recursive term_ that, in the SQL above, is written simply as `e.node_1 = terminal(p.path)`, must now be written thus:

```
e.node_1 = terminal(p.path) or e.node_2 = terminal(p.path)
```
Further, the `SELECT` list item in the _non-recursive term_ that, in the SQL above, is written simply as `array[seed, node_2]`, must now be written thus:

```
array[
  seed,
  case
    when seed = node_1 then node_2
    when seed = node_2 then node_1
  end case]
```

And the `SELECT` list item in the _recursive term_ that, in the SQL above, is written simply as `p.path||e.node_2`, must now be written thus:

```
p.path
||
case
  when terminal(p.path) = e.node_1 then e.node_2
  when terminal(p.path) = e.node_2 then e.node_1
end case
```

A corresponding adjustment must be made in the predicate that prevents cycles. It would bring unacceptable clutter to write three similar `CASE` expressions explicitly in place and so an encapsulation function is preferred. First, do this:

##### `cr-other-node.sql`

```plpgsql
drop function if exists other_node(text, text, text) cascade;

create function other_node(this_node in text, node_1 in text, node_2 in text)
  returns text
  language plpgsql
as $body$
begin
  -- Will get "case not found" error if no match.
  case
    when this_node = node_1 then return node_2;
    when this_node = node_2 then return node_1;
  end case;
end;
$body$;
```

And now recreate the _"find_paths"_ procedure:

##### `cr-find-paths-with-nocycle-check-alt.sql`

```plpgsql
drop procedure if exists find_paths(text) cascade;

create procedure find_paths(seed in text)
  language plpgsql
as $body$
begin
  -- See "cr-find-paths-with-pruning.sql". This index demonstrates that
  -- no more than one path has been found to any particular terminal node.
  drop index if exists raw_paths_terminal_unq cascade;
  commit;
  delete from raw_paths;

  with
    recursive paths(path) as (
      select array[seed, other_node(seed, node_1, node_2)]
      from edges
      where
      node_1 = seed or node_2 = seed

      union all

      select p.path||other_node(terminal(p.path), e.node_1, e.node_2)
      from edges e, paths p
      where
      (e.node_1 = terminal(p.path) or e.node_2 = terminal(p.path))
      and not other_node(terminal(p.path), e.node_1, e.node_2) = any(p.path) -- <<<<< Prevent cycles.
      )
  insert into raw_paths(path)
  select path
  from paths;
end;
$body$;
```

Invoke it and observe the results just as was shown above:

```plpgsql
call find_paths(seed => 'n1');
\t on
select t from list_paths('raw_paths');
\t off
```

Compare the results from the two approaches. You will see that they are identical.

Even with the _"other_node()"_ function encapsulation of the `CASE` logic, the SQL for the _"edges"_ representation that avoids denormalization is unacceptably more verbose, and correspondingly harder to understand, than is the SQL for the denormalized _"edges"_ representation. The representation that this section uses, and the SQL that it brings, are therefore discarded.

But there is another, and more critical, reason to discard the approach that avoids denormalization in the _"edges"_ table: the execution plan.

It's obviously quicker to identify a single row with an identity predicate on a column with an optimal index for such a predicate than it is to identify the row using the `OR` combination of two predicates, only one of which, of course, will identify the row. Moreover, because the primary key is defined on the column list _"(node_1, node_2)"_ and because the SQL for the denormalized _"edges"_ table approach restricts only on the leading column, the execution plan can use the index that enforces the primary key constraint.

Look at the output of the `\d` metacommand for the _"edges"_ table:

```
 Column | Type | Collation | Nullable | Default 
--------+------+-----------+----------+---------
 node_1 | text |           | not null | 
 node_2 | text |           | not null | 
Indexes:
    "edges_pk" PRIMARY KEY, lsm (node_1 HASH, node_2)
Check constraints:
    "edges_chk" CHECK (node_1 <> node_2)
```

This output is for the approach that uses denormalization. The output for the table that avoids denormalization is identical except for the spelling of the constraint. It uses the `<` inequality operator whereas the output shown here uses the `<>` operator. The `HASH` index on the _"node_1"_ column reflects the LSM storage method (effectively an index-organized table) down in the Spanner-inspired distributed storage layer—which is as good as it gets for the identity predicate on _"node_1"_.

In contrast, the identity predicate on _"node_2"_ will have no index support, Of course, you could create a secondary index on the _"node_2"_ column. But such an index brings its own bad consequences for the performance of data-changing statements in a distributed SQL database.

## How to implement early path pruning

The design concept here is to implement the pseudocode (as specified in the [section that specifies the semantics of the `WITH` clause recursive substatement](../../recursive-with/#semantics)) for the SQL for the approach that uses the denormalized _"edges"_ table approach.

Then, because the intermediate data that is normally inaccessible to user-SQL is now exposed in ordinary tables, the pruning code can be added.

First re-define the _"edges_chk"_ constraint re-populate the _"edges"_ table as specified in the section [Graph traversal using the denormalized "edges" table design](#graph-traversal-using-the-denormalized-edges-table-design).

Next, drop and re-create the _"find_paths()"_ procedure like this:

##### `cr-find-paths-with-pruning.sql`

```plpgsql
drop procedure if exists find_paths(text, boolean) cascade;

create procedure find_paths(seed in text, prune in boolean)
  language plpgsql
as $body$
<<b>>declare
  n int not null := 0;
begin
  -- Will be created at the end when "prune" is true.
  drop index if exists raw_paths_terminal_unq cascade;
  commit;

  -- Emulate the non-recursive term.
  delete from raw_paths;
  delete from working_paths;

  insert into working_paths(path)
  select array[seed, e.node_2]
  from edges e
  where
  e.node_1 = seed;

  insert into raw_paths(path)
  select r.path from working_paths r;

  -- Emulate the recursive term.
  loop
    delete from temp_paths;
    insert into temp_paths(path)
    select w.path||e.node_2
    from edges e, working_paths w
    where
    e.node_1 = terminal(w.path)
    and not e.node_2 = any(w.path);  -- <<<<< Prevent cycles.

    get diagnostics n = row_count;
    exit when n < 1;

    if prune then
      delete from temp_paths
      where
      (
        -- Prune all but one path to each distinct new terminal.
        path not in (select min(path) from temp_paths group by terminal(path)) and
        terminal(path) in (select terminal(path) from temp_paths)
      )
      or
      (
        -- Prune newer (and therefore longer) paths to
        -- already-found terminals.
        terminal(path) in
        (
          select terminal(path)
          from raw_paths
        )
      );
    end if;

    delete from working_paths;
    insert into working_paths(path) select t.path from temp_paths t;
    insert into raw_paths (path) select t.path from temp_paths t;
  end loop;
  commit;

  if prune then
    create unique index raw_paths_terminal_unq on raw_paths(terminal(path));
    commit;
  end if;
end b;
$body$;
```

Notice how the tables are used:

- The _"raw_paths_ table is used in place of the in-memory representation of the in-progress eventual final result set that the implementation of the `WITH` clause recursive substatement uses.
- The _"temp_paths"_ and _"working_paths"_ tables implement the transient in-memory representations that are shown in the pseudocode that specifies how the `WITH` clause recursive substatement works. (See the section [Semantics](../../recursive-with/#semantics)) subsection of the main account of the [WITH clause recursive substatement](../../recursive-with/) ).
- The _"raw_paths_trg"_ trigger is not needed to support the implementation. Rather, its purpose is only pedagogic.

Notice that the early pruning logic is guarded by an `IF` test that does, or omits, the pruning according to the value supplied for a boolean input formal parameter.

- It implements a `DELETE` statement whose `FROM` item is the _"temp_paths"_ table. This logic simply cannot be expressed using an explicit `WITH` clause recursive substatement for the simple reason that its implementation does not expose the structure that the _"temp_paths"_ table emulates for manipulation by user-written SQL.
- The first leg of the pruning predicate eliminates all but the first path in the path sorting order to any particular node in the set that the current repetition of the _recursive term_ has found. This restriction is expressed in terms of the content of the _"temp_paths"_ table. Notice that the choice to retain the path that sorts first is the same choice that the _["filter_paths()"](../common-code/#cr-restrict-to-shortest-paths-sql)_ algorithm uses. This ensures a deterministic comparison between the result produced by the straightforward _"find_paths()"_ implementation that uses the `WITH` clause recursive substatement directly, followed by _"restrict_to_shortest_paths()"_, and the result produced by the early pruning implementation of _"find_paths()"_.
- The second leg of the pruning predicate eliminates paths that the current repetition has found whose terminal is identical to that of any path found by any of the previous repetitions. These previously found paths are necessarily shorter than those that the present repetition finds. (See the code that inspects the _"repeat_nr"_ column in the _"raw_paths"_ table, and the associated explanation of its observed content, below.) This restriction is expressed in terms of the content of the under-construction ultimate result set in the _"raw_paths"_ table—which also is not exposed by user-written SQL when the explicit `WITH` clause recursive substatement is used.

First invoke this version of _"find_paths()"_ without pruning:

```plpgsql
call find_paths(seed => 'n1', prune => false);
```

and confirm, using  _["list_paths()"](../common-code/#cr-list-paths-sql)_ that it produces the same set of paths as does the version of _"find_paths()"_ that the section [Graph traversal using the denormalized "edges" table design](#graph-traversal-using-the-denormalized-edges-table-design) described.

```plpgsql
\t on
select t from list_paths('raw_paths');
\t off
```

Now inspect the _"repeat_nr"_ column, thus:

```plpgsql
select
  repeat_nr as "repeat #",
  path
from raw_paths
order by repeat_nr, path[2], path[3], path[4], path[5], path[6];
```
This is the result:

```
 repeat # |        path         
----------+---------------------
        0 | {n1,n2}

        1 | {n1,n2,n3}
        1 | {n1,n2,n4}

        2 | {n1,n2,n3,n5}
        2 | {n1,n2,n4,n5}
        2 | {n1,n2,n4,n6}

        3 | {n1,n2,n3,n5,n4}
        3 | {n1,n2,n3,n5,n6}
        3 | {n1,n2,n4,n5,n3}
        3 | {n1,n2,n4,n5,n6}
        3 | {n1,n2,n4,n6,n5}

        4 | {n1,n2,n3,n5,n4,n6}
        4 | {n1,n2,n3,n5,n6,n4}
        4 | {n1,n2,n4,n6,n5,n3}
```

The whitespace was added manually to separate the paths into groups that were produced by the same repeat of the recursive (or iterative, if you prefer) logic.

It confirms what you can, with sufficient intellectual effort, predict. But it's always sensible to confirm (or at least, fail to refute) such predictions by empirical observation.

> **Each successive repeat of the _recursive term_ produces a set of paths each of which is one step longer than those produced by the previous repeat**.

The design of the early pruning algorithm relies on this fact.

Next invoke it _with_ pruning and confirm that this produces the same set of paths that were produced by _["restrict_to_shortest_paths()"](../common-code/#cr-restrict-to-shortest-paths-sql)_ on the raw results produced by the version of _"find_paths()"_ that the section [Graph traversal using the denormalized "edges" table design](#graph-traversal-using-the-denormalized-edges-table-design) described.

It's better to do this by executing a programmatic assertion than simply by eyeballing two nominally identical result sets.

First, recreate the basic _"find_paths()"_ procedure by executing the code shown at [`cr-find-paths-with-nocycle-check.sql`](#cr-find-paths-with-nocycle-check-sql). (This is the version that uses the `WITH` clause recursive statement straightforwardly and that cannot, therefore, implement early pruning). Then invoke it.

```plpgsql
call find_paths(seed => 'n1');
call restrict_to_shortest_paths('raw_paths', 'shortest_paths');
```

Now reinstate the version that implements early pruning by executing the code shown at [`cr-find-paths-with-pruning.sql`](#cr-find-paths-with-pruning-sql) and invoke this.

```plpgsql
call find_paths(seed => 'n1', prune => true);
```
At this stage, the _"shortest_paths"_ table contains the set of shortest paths from node _"n1"_ produced by _post factum_ filtering of the set of all paths from _"n1"_. And the _"raw_paths"_ table contains the set of shortest paths from node _"n1"_ produced by early pruning. These two sets ought to be identical. Test this assertion with the procedure "[_assert_shortest_paths_same_as_raw_paths()_](../common-code/#cr-assert-shortest-paths-same-as-raw-paths-sql)":

```plpgsql
call assert_shortest_paths_same_as_raw_paths();
```

The assertion holds.
