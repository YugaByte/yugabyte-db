---
title: ALTER DOMAIN statement [YSQL]
headerTitle: ALTER DOMAIN
linkTitle: ALTER DOMAIN
description: Use the ALTER DOMAIN statement to change the definition of an existing domain.
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-commands-alter-domain
    parent: api-ysql-commands
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER DOMAIN` statement to change the definition of an existing domain.

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
    {{% includeMarkdown "../syntax_resources/commands/alter_domain_default,alter_domain_rename.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/alter_domain_default,alter_domain_rename.diagram.md" /%}}
  </div>
</div>

## Semantics

### SET DEFAULT | DROP DEFAULT

Set or remove the default value for a domain.

### RENAME

Change the name of the domain.

### *name*

Specify the name of the domain. An error is raised if DOMAIN `name` does not exist or DOMAIN `new_name` already exists.

## Examples

```postgresql
yugabyte=# CREATE DOMAIN idx DEFAULT 5 CHECK (VALUE > 0);
```

```postgresql
yugabyte=# ALTER DOMAIN idx DROP DEFAULT;
```

```postgresql
yugabyte=# ALTER DOMAIN idx RENAME TO idx_new;
```

```postgresql
yugabyte=# DROP DOMAIN idx_new;
```

## See also

- [`CREATE DOMAIN`](../ddl_create_domain)
- [`DROP DOMAIN`](../ddl_drop_domain)
