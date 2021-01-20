---
title: Using a WITH clause recursive substatement to traverse a directed acyclic graph
headerTitle: Finding the paths in a directed acyclic graph
linkTitle: directed acyclic graph
description: This section shows how to use a WITH clause recursive substatement to traverse a directed acyclic graph.
menu:
  latest:
    identifier: directed-acyclic-graph
    parent: traversing-general-graphs
    weight: 50
isTocNested: true
showAsideToc: true
---

Before trying the code in this section, make sure that you have created the _"edges"_ table (see [`cr-edges.sql`](../graph-representation/#cr-edges-sql)) and installed all the code shown in the section [Common code for traversing all kinds of graph](../common-code/).

First, define a suitable constraint on the _"edges_" table for representing a directed cyclic graph and populate the table with the data that represents the graph shown in the [Directed acyclic graph](../../traversing-general-graphs/#directed-acyclic-graph) section.

```plpgsql
delete from edges;
alter table edges drop constraint if exists edges_chk cascade;
alter table edges add constraint edges_chk check(node_1 <> node_2);

insert into edges(node_1, node_2) values
  ('n1', 'n2'),
  ('n1', 'n3'),
  ('n2', 'n5'),
  ('n2', 'n6'),
  ('n3', 'n6'),
  ('n4', 'n3'),
  ('n4', 'n7'),
  ('n3', 'n7');
```

Now create a simpler implementation of _"find_paths()"_ that omits the cycle prevention code. (It's derived trivially from the code shown at [`cr-find-paths-no-nocycle-check.sql`](../undirected-cyclic-graph/#cr-find-paths-no-nocycle-check-sql).)

##### `cr-find-paths-no-nocycle-check.sql`

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
      )
  insert into raw_paths(path)
  select path
  from paths;
end;
$body$;
```

Find all the paths from _"n1"_ and create the filtered subset of shortest paths to the distinct terminal nodes:

```plpgsql
call find_paths(seed => 'n1');
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
      1             2   n1 > n2
      2             2   n1 > n3
      3             3   n1 > n2 > n5
      4             3   n1 > n2 > n6
      5             3   n1 > n3 > n6
      6             3   n1 > n3 > n7
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
      1             2   n1 > n2
      2             2   n1 > n3
      3             3   n1 > n2 > n5
      4             3   n1 > n2 > n6
      5             3   n1 > n3 > n7
```

Notice that only the first (in sorting order) of the two paths to the same node, _"n1 > n2 > n6"_ and _"n1 > n3 > n6"_, has been retained.

Finally, add the shortest paths starting at each of the remaining nodes into the _"shortest_paths"_ table and list the final result.

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

\t on
select t from list_paths('shortest_paths');
\t off
```

This is the result:

```
 path #   cardinality   path
 ------   -----------   ----
      1             2   n1 > n2
      2             2   n1 > n3
      3             3   n1 > n2 > n5
      4             3   n1 > n2 > n6
      5             3   n1 > n3 > n7
      6             2   n2 > n5
      7             2   n2 > n6
      8             2   n3 > n6
      9             2   n3 > n7
     10             2   n4 > n3
     11             2   n4 > n7
     12             3   n4 > n3 > n6
```
