---
title: DROP DOMAIN statement [YSQL]
headerTitle: DROP DOMAIN
linkTitle: DROP DOMAIN
description: Use the DROP DOMAIN statement to remove a domain from the database.
menu:
  latest:
    identifier: ddl_drop_domain
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_drop_domain/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP DOMAIN` statement to remove a domain from the database.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_domain.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_domain.diagram.md" /%}}
  </div>
</div>

## Semantics

### *drop_domain*

#### *name*

Specify the name of the domain. An error is raised if the specified domain does not exist (unless `IF EXISTS` is set). An error is raised if any objects depend on this domain (unless `CASCADE` is set).

### IF EXISTS

Do not throw an error if the domain does not exist.

### CASCADE

Automatically drop objects that depend on the domain such as table columns using the domain data type and, in turn, all other objects that depend on those objects.

### RESTRICT

Refuse to drop the domain if objects depend on it (default).

## Examples

### Example 1

```plpgsql
yugabyte=# CREATE DOMAIN idx DEFAULT 5 CHECK (VALUE > 0);
```

```plpgsql
yugabyte=# DROP DOMAIN idx;
```

### Example 2

```plpgsql
yugabyte=# CREATE DOMAIN idx DEFAULT 5 CHECK (VALUE > 0);
```

```plpgsql
yugabyte=# CREATE TABLE t (k idx primary key);
```

```plpgsql
yugabyte=# DROP DOMAIN idx CASCADE;
```

## See also

- [`ALTER DOMAIN`](../ddl_alter_domain)
- [`CREATE DOMAIN`](../ddl_create_domain)
