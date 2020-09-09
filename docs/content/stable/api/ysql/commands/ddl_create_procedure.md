---
title: CREATE PROCEDURE statement [YSQL]
headerTitle: CREATE PROCEDURE
linkTitle: CREATE PROCEDURE
description: Use the CREATE PROCEDURE statement to define a new procedure in a database.
block_indexing: true
menu:
  stable:
    identifier: api-ysql-commands-create-procedure
    parent: api-ysql-commands
aliases:
  - /stable/api/ysql/ddl_create_procedure/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE PROCEDURE` statement to define a new procedure in a database.

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
    {{% includeMarkdown "../syntax_resources/commands/create_procedure,arg_decl.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/create_procedure,arg_decl.diagram.md" /%}}
  </div>
</div>

## Semantics

- If a procedure with the given `name` and argument types already exists then `CREATE PROCEDURE` will throw an error unless the `CREATE OR REPLACE PROCEDURE` version is used. 
    In that case it will replace any existing definition instead.

- The languages supported by default are `sql`, `plpgsql` and `C`.


## Examples

- Set up an accounts table.
    ```postgresql
    CREATE TABLE accounts (
      id integer PRIMARY KEY,
      name text NOT NULL,
      balance decimal(15,2) NOT NULL
    );

    INSERT INTO accounts VALUES (1, 'Jane', 100.00);
    INSERT INTO accounts VALUES (2, 'John', 50.00);

    SELECT * from accounts;
    ```

    ```
     id | name | balance
    ----+------+---------
      1 | Jane |  100.00
      2 | John |   50.00
    (2 rows)
    ```
- Define a `transfer` procedure to transfer money from one account to another.

    ```postgresql
    CREATE OR REPLACE PROCEDURE transfer(integer, integer, decimal)
    LANGUAGE plpgsql
    AS $$
    BEGIN
      IF $3 <= 0.00 then RAISE EXCEPTION 'Can only transfer positive amounts'; END IF;
      IF $1 = $2 then RAISE EXCEPTION 'Sender and receiver cannot be the same'; END IF;
      UPDATE accounts SET balance = balance - $3 WHERE id = $1;
      UPDATE accounts SET balance = balance + $3 WHERE id = $2;
      COMMIT;
    END;
    $$;
    ```

- Transfer `$20.00` from Jane to John.
```postgresql
CALL transfer(1, 2, 20.00);
SELECT * from accounts;
```

```
 id | name | balance
----+------+---------
  1 | Jane |   80.00
  2 | John |   70.00
(2 rows)
```

- Errors will be thrown for unsupported argument values.
```postgresql
CALL transfer(2, 2, 20.00);
```
```
ERROR:  Sender and receiver cannot be the same
CONTEXT:  PL/pgSQL function transfer(integer,integer,numeric) line 4 at RAISE
```

```postgresql
yugabyte=# CALL transfer(1, 2, -20.00);
```

```
ERROR:  Can only transfer positive amounts
CONTEXT:  PL/pgSQL function transfer(integer,integer,numeric) line 3 at RAISE
```

## See also

- [`CREATE FUNCTION`](../ddl_create_function)
- [`CREATE TRIGGER`](../ddl_create_trigger)
- [`DROP PROCEDURE`](../ddl_drop_procedure)
