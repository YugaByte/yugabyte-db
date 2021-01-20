---
title: CREATE FUNCTION statement [YSQL]
headerTitle: CREATE FUNCTION
linkTitle: CREATE FUNCTION
description: Use the CREATE FUNCTION statement to create a function in a database.
menu:
  latest:
    identifier: ddl_create_function
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_create_function/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE FUNCTION` statement to create a function in a database.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_function,arg_decl.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_function,arg_decl.diagram.md" /%}}
  </div>
</div>

## Semantics

- If a function with the given `name` and argument types already exists then `CREATE FUNCTION` will throw an error unless the `CREATE OR REPLACE FUNCTION` version is used. In that case it will replace the existing definition instead.

- The languages supported by default are `sql`, `plpgsql` and `C`.

- `VOLATILE`, `STABLE` and `IMMUTABLE` inform the query optimizer about the behavior the function. 
    - `VOLATILE` is the default and indicates that the function result could be different for every call. For instance `random()` or `now()`.
    - `STABLE` indicates that the function cannot modify the database so that within a single scan it will return the same result given the same arguments.
    - `IMMUTABLE` indicates that the function cannot modify the database _and_ always returns the same results given the same arguments.

- `CALLED ON NULL INPUT`, `RETURNS NULL ON NULL INPUT` and `STRICT` define the function's behavior with respect to 'null's.
    - `CALLED ON NULL INPUT` indicates that input arguments may be `null`.
    - `RETURNS NULL ON NULL INPUT` or `STRICT` indicate that the function always returns `null` if any of its arguments are `null`. 

## Examples

### Define a function using the SQL language.

```plpgsql
CREATE FUNCTION mul(integer, integer) RETURNS integer
    AS 'SELECT $1 * $2;'
    LANGUAGE SQL
    IMMUTABLE
    RETURNS NULL ON NULL INPUT;

SELECT mul(2,3), mul(10, 12);
```

```
 mul | mul
-----+-----
   6 | 120
(1 row)
```

### Define a function using the PL/pgSQL language.

```plpgsql
CREATE OR REPLACE FUNCTION inc(i integer) RETURNS integer AS $$
        BEGIN
                RAISE NOTICE 'Incrementing %', i ;
                RETURN i + 1;
        END;
$$ LANGUAGE plpgsql;

SELECT inc(2), inc(5), inc(10);
```

```
NOTICE:  Incrementing 2
NOTICE:  Incrementing 5
NOTICE:  Incrementing 10
 inc | inc | inc
-----+-----+-----
   3 |   6 |  11
(1 row)
```

## See also

- [`CREATE PROCEDURE`](../ddl_create_procedure)
- [`CREATE TRIGGER`](../ddl_create_trigger)
- [`DROP FUNCTION`](../ddl_drop_function)
