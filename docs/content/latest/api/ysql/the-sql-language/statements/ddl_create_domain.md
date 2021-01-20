---
title: CREATE DOMAIN statement [YSQL]
headerTitle: CREATE DOMAIN
linkTitle: CREATE DOMAIN
description: Use the CREATE DOMAIN statement to create a user-defined data type with optional constraints.
menu:
  latest:
    identifier: ddl_create_domain
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_create_domain/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE DOMAIN` statement to create a user-defined data type with optional constraints, such as range of valid values, `DEFAULT`, `NOT NULL`, and `CHECK`. Domains are useful to abstract data types with common constraints. For example, domain can be used to represent phone number columns that will require the same `CHECK` constraints on the syntax.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_domain,domain_constraint.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_domain,domain_constraint.diagram.md" /%}}
  </div>
</div>

## Semantics

### *create_domain*

### CREATE DOMAIN *name*

Specify the name of the domain. An error is raised if `name` already exists in the specified database.

### AS *data_type*

Specify the underlying data type.

### DEFAULT *expression*

Set the default value for columns of the domain data type.

### *domain_constraint*

#### CONSTRAINT *constraint_name*

Specify the optional name for the constraint.

##### NOT NULL

Do not allow null values.

##### NULL

Allow null values (default).

##### CHECK ( *expression* )

Enforce a constraint that the values of the domain must satisfy and returns a Boolean value.

The key word VALUE should be used to refer to the value being tested. Expressions evaluating to TRUE or UNKNOWN succeed.

## Examples

```plpgsql
yugabyte=# CREATE DOMAIN phone_number AS TEXT CHECK(VALUE ~ '^\d{3}-\d{3}-\d{4}$');
```

```plpgsql
yugabyte=# CREATE TABLE person(first_name TEXT, last_name TEXT, phone_number phone_number);
```

## See also

- [`DROP DOMAIN`](../ddl_drop_domain)
- [`ALTER DOMAIN`](../ddl_alter_domain)
