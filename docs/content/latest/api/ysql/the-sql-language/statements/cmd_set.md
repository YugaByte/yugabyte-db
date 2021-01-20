---
title: SET statement [YSQL]
headerTitle: SET
linkTitle: SET 
description: Use the SET statement to update a run-time control parameter.
menu:
  latest:
    identifier: cmd_set
    parent: statements
aliases:
  - /latest/api/ysql/commands/cmd_set/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `SET` statement to update a run-time control parameter.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/set.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/set.diagram.md" /%}}
  </div>
</div>

## Semantics

Although the values of a parameter can be set, displayed, and reset, the effect of these parameters are not yet supported in YugabyteDB. The default settings and behaviors will be used for the moment.

### SESSION

Specify that the command affects only the current session.

### LOCAL

Specify that the command affects only the current transaction. After `COMMIT` or `ROLLBACK`, the session-level setting takes effect again.

### *configuration_parameter*

Specify the name of a mutable run-time parameter.

### value

Specify the value of parameter.

## See also

- [`SHOW`](../cmd_show)
- [`RESET`](../cmd_reset)
