---
title: INSERT statement [YSQL]
headerTitle: INSERT
linkTitle: INSERT
description: Use the INSERT statement to add one or more rows to the specified table.
block_indexing: true
menu:
  stable:
    identifier: api-ysql-commands-insert
    parent: api-ysql-commands
aliases:
  - /stable/api/ysql/dml_insert/
  - /stable/api/postgresql/dml_insert/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `INSERT` statement to add one or more rows to the specified table.

## Syntax

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
    {{% includeMarkdown "../syntax_resources/commands/insert,returning_clause,column_values,conflict_target,conflict_action.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/insert,returning_clause,column_values,conflict_target,conflict_action.diagram.md" /%}}
  </div>
</div>

## Semantics

Constraints must be satisfied.  

### *insert*

### *table_name*

Specify the name of the table. If the specified table does not exist, an error is raised.

### *column_names*

Specify a comma-separated list of columns names. If a specified column does not exist, an error is raised. Each of the primary key columns must have a non-null value.

### `VALUES` clause

- Each of the values list must have the same length as the columns list.
- Each value must be convertible to its corresponding (by position) column type.
- Each value literal can be an expression.

### `ON CONFLICT` clause

- The target table must have at least one column (list) with either a unique index
or a unique constraint. We shall refer to this as a unique key. The argument of VALUES
is a relation that must include at least one of the target table's unique keys.
Some of the values of this unique key might be new, and others might already exist
in the target table.

- The basic aim of INSERT ON CONFLICT is simply to insert the rows with new values of
the unique key and to update the rows with existing values of the unique key to
set the values of the remaining specified columns to those in the VALUES relation.
In this way, the net effect is either to insert or to update; and for this reason
the INSERT ON CONFLICT variant is often colloquially referred to as "upsert".

### *returning_clause*

### *column_values*

### *conflict_target*

### *conflict_action*

```
DO NOTHING | DO UPDATE SET *update_item* [ , ... ] [ WHERE *condition* ]
```

#### *update_item*

#### *condition*

## Examples

First, the bare insert. Create a sample table.

```postgresql
yugabyte=# CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

Insert some rows.

```postgresql
yugabyte=# INSERT INTO sample VALUES (1, 2.0, 3, 'a'), (2, 3.0, 4, 'b'), (3, 4.0, 5, 'c');
```

Check the inserted rows.

```postgresql
yugabyte=# SELECT * FROM sample ORDER BY k1;
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  2 |  3 |  4 | b
  3 |  4 |  5 | c
```

Next, a basic "upsert" example. Re-create and re-populate the sample table.

```postgresql
yugabyte=# DROP TABLE IF EXISTS sample CASCADE;
```

```postgresql
yugabyte=# CREATE TABLE sample(
  id int  CONSTRAINT sample_id_pk PRIMARY KEY,
  c1 text CONSTRAINT sample_c1_NN NOT NULL,
  c2 text CONSTRAINT sample_c2_NN NOT NULL);
```

```postgresql
yugabyte=# INSERT INTO sample(id, c1, c2)
  VALUES (1, 'cat'    , 'sparrow'),
         (2, 'dog'    , 'blackbird'),
         (3, 'monkey' , 'thrush');
```

Check the inserted rows.

```postgresql
yugabyte=# SELECT id, c1, c2 FROM sample ORDER BY id;
```

```
 id |   c1   |    c2å
----+--------+-----------
  1 | cat    | sparrow
  2 | dog    | blackbird
  3 | monkey | thrush
```

Demonstrate "on conflict do nothing". In this case, you don't need to specify the conflict target.

```postgresql
yugabyte=# INSERT INTO sample(id, c1, c2)
  VALUES (3, 'horse' , 'pigeon'),
         (4, 'cow'   , 'robin')
  ON CONFLICT
  DO NOTHING;
```

Check the result.
The non-conflicting row with id = 4 is inserted, but the conflicting row with id = 3 is NOT updated.

```postgresql
yugabyte=# SELECT id, c1, c2 FROM sample ORDER BY id;
```

```
 id |   c1   |    c2
----+--------+-----------
  1 | cat    | sparrow
  2 | dog    | blackbird
  3 | monkey | thrush
  4 | cow    | robin
```

Demonstrate the real "upsert". In this case, you DO need to specify the conflict target. Notice the use of the
EXCLUDED keyword to specify the conflicting rows in the to-be-upserted relation.

```postgresql
yugabyte=# INSERT INTO sample(id, c1, c2)
  VALUES (3, 'horse' , 'pigeon'),
         (5, 'tiger' , 'starling')
  ON CONFLICT (id)
  DO UPDATE SET (c1, c2) = (EXCLUDED.c1, EXCLUDED.c2);

```

Check the result.
The non-conflicting row with id = 5 is inserted, and the conflicting row with id = 3 is updated.

```postgresql
yugabyte=# SELECT id, c1, c2 FROM sample ORDER BY id;
```

```
 id |  c1   |    c2
----+-------+-----------
  1 | cat   | sparrow
  2 | dog   | blackbird
  3 | horse | pigeon
  4 | cow   | robin
  5 | tiger | starling
```

We can make the "update" happen only for a specified subset of the
excluded rows. We illustrate this by attempting to insert two conflicting rows
(with id = 4 and id = 5) and one non-conflicting row (with id = 6).
And you specify that the existing row with c1 = 'tiger' should not be updated
with "WHERE sample.c1 <> 'tiger'".

```postgresql
INSERT INTO sample(id, c1, c2)
  VALUES (4, 'deer'   , 'vulture'),
         (5, 'lion'   , 'hawk'),
         (6, 'cheeta' , 'chaffinch')
  ON CONFLICT (id)
  DO UPDATE SET (c1, c2) = (EXCLUDED.c1, EXCLUDED.c2)
  WHERE sample.c1 <> 'tiger';

```

Check the result.
The non-conflicting row with id = 6 is inserted;  the conflicting row with id = 4 is updated;
but the conflicting row with id = 5 (and c1 = 'tiger') is NOT updated;

```postgresql
yugabyte=# SELECT id, c1, c2 FROM sample ORDER BY id;
```

```
 id |   c1   |    c2     
----+--------+-----------
  1 | cat    | sparrow
  2 | dog    | blackbird
  3 | horse  | pigeon
  4 | deer   | vulture
  5 | tiger  | starling
  6 | cheeta | chaffinch
```

Notice that this restriction is legal too:

```
WHERE EXCLUDED.c1 <> 'lion'
```

Finally, a slightly more elaborate "upsert" example. Re-create and re-populate the sample table.
Notice that id is a self-populating surrogate primary key and that c1 is a business unique key.

```postgresql
yugabyte=# DROP TABLE IF EXISTS sample CASCADE;
```

```postgresql
CREATE TABLE sample(
  id INTEGER GENERATED ALWAYS AS IDENTITY CONSTRAINT sample_id_pk PRIMARY KEY,
  c1 TEXT CONSTRAINT sample_c1_NN NOT NULL CONSTRAINT sample_c1_unq unique,
  c2 TEXT CONSTRAINT sample_c2_NN NOT NULL);
```

```postgresql
INSERT INTO sample(c1, c2)
  VALUES ('cat'   , 'sparrow'),
         ('deer'  , 'thrush'),
         ('dog'   , 'blackbird'),
         ('horse' , 'vulture');
```

Check the inserted rows.

```postgresql
yugabyte=# SELECT id, c1, c2 FROM sample ORDER BY c1;
```

```
 id |  c1   |    c2
----+-------+-----------
  1 | cat   | sparrow
  2 | deer  | thrush
  3 | dog   | blackbird
  4 | horse | vulture
```

Now do the upsert. Notice that this illustrates the usefulness
of the WITH clause to define the to-be-upserted relation
before the INSERT clause and use a subselect instead of
a VALUES clause. We also specify the conflict columns
indirectly by mentioning the name of the unique constrained
that covers them.

```postgresql
yugabyte=# WITH to_be_upserted AS (
  SELECT c1, c2 FROM (VALUES
    ('cat'   , 'chaffinch'),
    ('deer'  , 'robin'),
    ('lion'  , 'duck'),
    ('tiger' , 'pigeon')
   )
  AS t(c1, c2)
  )
  INSERT INTO sample(c1, c2) SELECT c1, c2 FROM to_be_upserted
  ON CONFLICT ON CONSTRAINT sample_c1_unq
  DO UPDATE SET c2 = EXCLUDED.c2;
```

Check the inserted rows.

```postgresql
yugabyte=# SELECT id, c1, c2 FROM sample ORDER BY c1;
```

```
 id |  c1   |    c2
----+-------+-----------
  1 | cat   | chaffinch
  2 | deer  | robin
  3 | dog   | blackbird
  4 | horse | vulture
  7 | lion  | duck
  8 | tiger | pigeon
```

## See also

- [`COPY`](../cmd_copy)
- [`CREATE TABLE`](../ddl_create_table)
- [`SELECT`](../dml_select)
