---
title: ALTER DATABASE statement [YSQL]
headerTitle: ALTER DATABASE
linkTitle: ALTER DATABASE
description: Use the ALTER DATABASE statement to redefine the attributes of a database.
menu:
  latest:
    identifier: ddl_alter_db
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_alter_db/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER DATABASE` statement to redefine the attributes of a database.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_database,alter_database_option.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_database,alter_database_option.diagram.md" /%}}
  </div>
</div>

## Semantics

{{< note title="Note" >}}

Some options in DATABASE are under development.

{{< /note >}}

### *name*

Specify the name of the database to be altered.

### ALLOW_CONNECTIONS

Specify `false` to disallow connections to this database. Default is `true`, which allows this database to be cloned by any user with `CREATEDB` privileges.

### CONNECTION_LIMIT

Specify how many concurrent connections can be made to this database. Default of `-1` allows unlimited concurrent connections.

### IS_TEMPLATE

S`true` — This database can be cloned by any user with `CREATEDB` privileges.
Specify `false` to Only superusers or the owner of the database can clone it.

## See also

- [`CREATE DATABASE`](../ddl_create_database)
- [`DROP DATABASE`](../ddl_drop_database)
- [`SET`](../cmd_set)
