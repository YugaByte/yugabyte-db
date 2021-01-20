---
title: Representing a graph in a SQL database
headerTitle: Representing different kinds of graph in a SQL database
linkTitle: graph representation
description: This section shows how to represent different kinds of graph in a SQL database,
menu:
  latest:
    identifier: graph-representation
    parent: traversing-general-graphs
    weight: 10
isTocNested: true
showAsideToc: true
---

## Representing a graph in a SQL database

Look at the diagram of an example [undirected cyclic graph](../#undirected-cyclic-graph) on this page's parent page. You can see that it has seven edges thus:

> _(n1-n2), (n2-n3), (n2-n4), (n3-n5), (n4-n5), (n4-n6), (n5-n6)_

The minimal representation in a SQL database uses a just table of table of edges, created thus:

##### `cr-edges.sql`

```plpgsql
drop table if exists edges cascade;

create table edges(
  node_1 text not null,
  node_2 text not null,
  constraint edges_pk primary key(node_1, node_2));
```

A typical special case, like [computing Bacon Numbers](../bacon-numbers/), would specify _"node_1"_ and _"node_2"_ with names that are specific to the use case (like _"actor"_ and _"co_actor"_) whose values are the primary keys in a separate _"actors"_ table. This implies foreign key constraints, of course. The _"actors"_ table would have other columns (like "_given_name"_, _"family_name"_, and so on). Similarly, the _"edges"_ table would have at least one other column: the array of movies that the two connected actors have been in.

## How to represent the undirected nature of the edges

There are two obvious design choices. One design populates the _"edges"_ table sparsely by recording each edge just once. For example, if node _"a"_ is recorded in column _"node_1"_, then it must not be recorded in column _"node_2"_. Then you must understand that though the column names _"node_1"_ and _"node_2"_ imply a direction, this is not the case.

Though this design has the appeal of avoiding denormalization, it makes the SQL design tricky. When the traversal arrives at a node and needs to find the nodes at the other ends of the edges that have the present node at one of their ends, you don't know whether to restrict on _"node_1"_ and read _"node_2"_ or to restrict on _"node_2"_ and read _"node_1"_ and so you must try both.

The other design records each edge twice. For example, the edge between nodes _"a"_ and _"b"_ will be recorded both as _(node_1 = 'a', node_2 = 'b')_ and as _(node_1 = 'b', node_2 = 'a')_. This denormalization makes the SQL straightforward—and is therefore preferred.

An implementation of the [design that represents each edge just once](../undirected-cyclic-graph/#graph-traversal-using-the-normalized-edges-table-design) is described for completeness. But the approach that leads to simpler SQL, the [design that represents each edge twice—once in each direction](../undirected-cyclic-graph/#graph-traversal-using-the-denormalized-edges-table-design) is described first.

The representations for the more specialized kinds of graph—[directed cyclic graph](../#directed-cyclic-graph), [directed acyclic graph](../#directed-acyclic-graph), and [rooted tree](../#rooted-tree)—all use the same table structure but with appropriately different rules to which the content must conform.
