---
title: Using a WITH clause recursive substatement to traverse a rooted tree
headerTitle: Finding the paths in a rooted tree
linkTitle: rooted tree
description: This section shows how to use a WITH clause recursive substatement to traverse a rooted tree.
menu:
  latest:
    identifier: rooted-tree
    parent: traversing-general-graphs
    weight: 60
isTocNested: true
showAsideToc: true
---

Before trying the code in this section, make sure that you have created the _"edges"_ table (see [`cr-edges.sql`](../graph-representation/#cr-edges-sql)) and installed all the code shown in the section [Common code for traversing all kinds of graph](../common-code/).

Notice that, because a rooted tree is such a drastically restricted specialization of the general graph, it allows a simpler representation where only the nodes are explicitly represented and where the edges are inferred. This is explained in the section [Using a WITH clause recursive substatement to traverse a hierarchy](../../emps-hierarchy/). It's unlikely, therefore, that you'd represent a rooted tree in the way that the code presented in this section is written for. It is, however, important to show that the general approach for graph traversal does indeed handle all the restricted kinds of graph.

First, define a suitable constraint on the _"edges_" table for representing a directed cyclic graph and populate the table with the data that represents the graph shown in the [Rooted tree](../../traversing-general-graphs/#rooted-tree) section.

```plpgsql
delete from edges;
alter table edges drop constraint if exists edges_chk cascade;
alter table edges add constraint edges_chk check(node_1 <> node_2);

insert into edges(node_1, node_2) values
  ('n01', 'n02'),
  ('n01', 'n03'),
  ('n01', 'n04'),
  ('n02', 'n05'),
  ('n02', 'n06'),
  ('n05', 'n11'),
  ('n05', 'n12'),
  ('n03', 'n07'),
  ('n03', 'n08'),
  ('n04', 'n09'),
  ('n04', 'n10'),
  ('n10', 'n13'),
  ('n10', 'n14');
```

Now re-create the simpler implementation of _"find_paths()"_, shown at [`cr-find-paths-no-nocycle-check.sql`](../directed-acyclic-graph/#cr-find-paths-no-nocycle-check-sql), that omits the cycle prevention code.

Because the root is uniquely defined, it can be determined mechanically. Do this:

```plpgsql
do $body$
declare
  root constant text not null := (
    select distinct node_1 from edges
    where node_1 not in (
      select node_2 from edges)
  );
begin
  call find_paths(seed => root);
end;
$body$;

\t on
select t from list_paths('raw_paths');
\t off
```

This is the result:

```
 path #   cardinality   path
 ------   -----------   ----
      1             2   n01 > n02
      2             2   n01 > n03
      3             2   n01 > n04
      4             3   n01 > n02 > n05
      5             3   n01 > n02 > n06
      6             3   n01 > n03 > n07
      7             3   n01 > n03 > n08
      8             3   n01 > n04 > n09
      9             3   n01 > n04 > n10
     10             4   n01 > n02 > n05 > n11
     11             4   n01 > n02 > n05 > n12
     12             4   n01 > n04 > n10 > n13
     13             4   n01 > n04 > n10 > n14
```

A rooted tree has only one path to any node, so calling the procedure _"restrict_to_shortest_paths()"_ will not remove any paths. Confirm this using the same procedure _["assert_shortest_paths_same_as_raw_paths()"](../common-code/#cr-assert-shortest-paths-same-as-raw-paths-sql)_ that was used in the section [How to implement early path pruning](../undirected-cyclic-graph/#how-to-implement-early-path-pruning) to show that the version of _"find_paths()"_ that implements early pruning produces the same set of paths as does the implementation that uses the `WITH` clause recursive substatement ordinarily followed by a call to _"restrict_to_shortest_paths()"_:

```plpgsql
call restrict_to_shortest_paths('raw_paths', 'shortest_paths');
call assert_shortest_paths_same_as_raw_paths();
```

The assertion holds.

