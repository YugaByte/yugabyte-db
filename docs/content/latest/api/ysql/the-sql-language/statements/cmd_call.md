---
title: CALL statement [YSQL]
headerTitle: CALL
linkTitle: CALL
description: Use the CALL statement to execute a stored procedure.
menu:
  latest:
    identifier: cmd_call
    parent: statements
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CALL` statement to execute a stored procedure.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/call_procedure,procedure_argument,argument_name.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/call_procedure,procedure_argument,argument_name.diagram.md" /%}}
  </div>
</div>
**Note:** The syntax and semantics of the `procedure_argument` (for example how to use the named parameter invocation style to avoid providing actual arguments for defaulted parameters) is the same for invoking a user-defined`FUNCTION`. A function cannot be invoked with the `CALL` statement. Rather, it's invoked as (part of) an expression in DML statements like `SELECT`.


## Semantics

`CALL` executes a stored procedure. If the procedure has any output parameters, then a result row will be returned, containing the values of those parameters.

The caller must have _both_ the `USAGE` privilege on the schema in which the to-be-called procedure exists _and_ the  `EXECUTE` privilege on it. If the caller lacks the required `USAGE` privilege, then it causes this error:

```
42501: permission denied for schema %"
```

If the caller has the required `USAGE` privilege but lacks the required `EXECUTE` privilege, then it causes this error:

```
42501: permission denied for procedure %
```

## Notes

If `CALL` is executed in a transaction block, then it cannot execute transaction control statements. The attempt causes this runtime error:

```
2D000: invalid transaction termination
```

Transaction control statements are  allowed when `CALL` is invoked with `AUTOCOMMIT` set to `on`—in which case the procedure executes in its own transaction.

## Example

Create a simple procedure

```plpgsql
set client_min_messages = warning;
drop procedure if exists p(text, int) cascade;
create procedure p(
  caption in text default 'Caption',
  int_val in int default 17)
  language plpgsql
as $body$
begin
  raise info 'Result: %: %', caption, int_val::text;
end;
$body$;
```

Invoke it using the simple syntax:

```plpgsql
call p('Forty-two', 42);
```
This is the result:

```
INFO:  Result: Forty-two: 42
```
Omit the second defaulted parameter:

```plpgsql
call p('"int_val" default is');
```

This is the result:

```
INFO:  Result: "int_val" default is: 17
```

Omit the both defaulted parameters:

```plpgsql
call p();
```

This is the result:

```
INFO:  Result: Caption: 17
```

Invoke it by using the names of the formal parameters.

```plpgsql
call p(caption => 'Forty-two', int_val=>42);
```

This is the result:

```
INFO:  Result: Forty-two: 42
```

Invoke it by just the named second parameter.

```plpgsql
call p(int_val=>99);
```

This is the result:

```
INFO:  Result: Caption: 99
```

Provoke an error by using just the second unnamed parameter.

```plpgsql
call p(99);
```

It causes this error:

```
42883: procedure p(integer) does not exist
```
In this case, this generic hint:

```
You might need to add explicit type casts.
```

isn't helpful.

Create a procedure procedure with `INOUT` formal parameters.

```plpgsql
drop procedure if exists x(int, int, int, int, int);
create procedure x(
  a inout int,
  b inout int,
  c inout int,
  d inout int,
  e inout int)
  language plpgsql
as $body$
begin
  a := a + 1;
  b := b + 2;
  c := c + 3;
  d := d + 4;
  e := e + 5;
end;
$body$;
```

Notice that this is rather strange. The ordinary meaning of an `INOUT` parameter is that its value will be changed by the invocation so that the caller sees different values after the call. Normally, a procedure designed and written to be called from another procedure or a `DO` block with actual arguments that are declared as variables. But when such a procedure is called using a top-level `CALL` statement, it returns the results in the same way that a `SELECT` statement does. Try this:

```plpgsql
call x(10, 20, 30, 40, 50);
```
This is the result:

```
 a  | b  | c  | d  | e  
----+----+----+----+----
 11 | 22 | 33 | 44 | 55
```
You cannot create a procedure with `OUT` formal parameters. The attempt causes the error

```
0A000: procedures cannot have OUT arguments
```

