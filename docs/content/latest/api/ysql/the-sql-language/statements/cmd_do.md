---
title: DO statement [YSQL]
headerTitle: DO
linkTitle: DO
description: Use the DO statement to execute an anonymous code block or, in other words, a transient anonymous function in a procedural language.
menu:
  latest:
    identifier: cmd_do
    parent: statements
aliases:
  - /latest/api/ysql/commands/cmd_do/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DO` statement to execute an anonymous code block, or in other words, a transient anonymous function in a procedural language.
The code block is treated as though it were the body of a function with no parameters, returning void. It is parsed and executed a single time.
The optional `LANGUAGE` clause can be written either before or after the code block.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/do.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/do.diagram.md" /%}}
  </div>
</div>

## Semantics

### *code*
The procedural language code to be executed. This must be specified as a string literal, just as in `CREATE FUNCTION`. Use of a dollar-quoted literal is recommended.

### *lang_name*
The name of the procedural language the code is written in. If omitted, the default is `plpgsql`.
The procedural language to be used must already have been installed into the current database. `plpgsql` is installed by default, but other languages are not.

## Notes

If `DO` is executed in a transaction block, then the procedure code cannot execute transaction control statements. The attempt causes this runtime error:

```
2D000: invalid transaction termination
```

Transaction control statements are  allowed with `AUTOCOMMIT` set to `on`—in which case the `DO` statement executes in its own transaction.

## Examples

```plpgsql
DO $$DECLARE r record;
BEGIN
    FOR r IN SELECT table_schema, table_name FROM information_schema.tables
             WHERE table_type = 'VIEW' AND table_schema = 'public'
    LOOP
        EXECUTE 'GRANT ALL ON ' || quote_ident(r.table_schema) || '.' || quote_ident(r.table_name) || ' TO webuser';
    END LOOP;
END$$;
```
