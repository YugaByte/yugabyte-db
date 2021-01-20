---
title: Using a WITH clause recursive substatement to traverse a directed cyclic graph
headerTitle: Finding the paths in a directed cyclic graph
linkTitle: directed cyclic graph
description: This section shows how to use a WITH clause recursive substatement to traverse a directed cyclic graph.
menu:
  latest:
    identifier: directed-cyclic-graph
    parent: traversing-general-graphs
    weight: 40
isTocNested: true
showAsideToc: true
---

Before trying the code in this section, make sure that you have created the _"edges"_ table (see [`cr-edges.sql`](../graph-representation/#cr-edges-sql)) and installed all the code shown in the section [Common code for traversing all kinds of graph](../common-code/).

First, define a suitable constraint on the _"edges_" table for representing a directed cyclic graph and populate the table with the data that represents the graph shown in the [Directed cyclic graph](../../traversing-general-graphs/#directed-cyclic-graph) section.

```plpgsql
delete from edges;
alter table edges drop constraint if exists edges_chk cascade;
alter table edges add constraint edges_chk check(node_1 <> node_2);

insert into edges(node_1, node_2) values
  ('n2', 'n1'),
  ('n2', 'n4'),
  ('n4', 'n5'),
  ('n4', 'n6'),
  ('n6', 'n4'),
  ('n5', 'n6'),
  ('n5', 'n3'),
  ('n3', 'n2');
```

Now reinstate the implementation of _"find_paths()"_ shown at [cr-find-paths-with-nocycle-check.sql](../undirected-cyclic-graph/#cr-find-paths-with-nocycle-check-sql).

Find all the paths from _"n2"_ and create the filtered subset of shortest paths to the distinct terminal nodes:

```plpgsql
call find_paths(seed => 'n2');
call restrict_to_shortest_paths('raw_paths', 'shortest_paths');
```

Look at the _"raw_paths"_:

```plpgsql
\t on
select t from list_paths('raw_paths');
\t off
```

This is the result:

```
 path #   cardinality   path
 ------   -----------   ----
      1             2   n2 > n1
      2             2   n2 > n4
      3             3   n2 > n4 > n5
      4             3   n2 > n4 > n6
      5             4   n2 > n4 > n5 > n3
      6             4   n2 > n4 > n5 > n6
```
Look at the "_filtered_paths"_:

```plpgsql
\t on
select t from list_paths('shortest_paths');
\t off
```

This is the result:

```
 path #   cardinality   path
 ------   -----------   ----
      1             2   n2 > n1
      2             2   n2 > n4
      3             3   n2 > n4 > n5
      4             3   n2 > n4 > n6
      5             4   n2 > n4 > n5 > n3
```
