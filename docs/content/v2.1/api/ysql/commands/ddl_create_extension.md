---
title: CREATE EXTENSION statement [YSQL]
headerTitle: CREATE EXTENSION
linkTitle: CREATE EXTENSION
summary: Create a new extension in a database
description: Use the CREATE EXTENSION statement to load an extension into a database.
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-commands-create-extension
    parent: api-ysql-commands
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE EXTENSION` statement to load an extension into a database.

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
    {{% includeMarkdown "../syntax_resources/commands/create_extension.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/create_extension.diagram.md" /%}}
  </div>
</div>

## Semantics

- `SCHEMA`, `VERSION`, and `CASCADE` may be reordered.

## Examples

```postgresql
CREATE SCHEMA myschema;
CREATE EXTENSION pgcrypto WITH SCHEMA myschema VERSION '1.3';
```

```
CREATE EXTENSION
```

```postgresql
CREATE EXTENSION IF NOT EXISTS earthdistance CASCADE;
```

```
NOTICE:  installing required extension "cube"
CREATE EXTENSION
```

## See also

- [Extensions page](../../extensions)
- [`DROP EXTENSION`](../ddl_drop_extension)
