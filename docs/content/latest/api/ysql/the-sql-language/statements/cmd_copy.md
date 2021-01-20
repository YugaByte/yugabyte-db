---
title: COPY statement [YSQL]
headerTitle: COPY
linkTitle: COPY
description: Transfer data between tables and files with the COPY, COPY TO, and COPY FROM statements.
menu:
  latest:
    identifier: cmd_copy
    parent: statements
aliases:
  - /latest/api/ysql/commands/cmd_copy/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `COPY` statement to transfer data between tables and files. `COPY TO` copies from tables to files. `COPY FROM` copies from files to tables. `COPY` outputs the number of rows that were copied.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/copy_from,copy_to,copy_option.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/copy_from,copy_to,copy_option.diagram.md" /%}}
  </div>
</div>

## Semantics

### *table_name*

Specify the table, optionally schema-qualified, to be copied.

### *column_name*

Specify the list of columns to be copied. If not specified, then all columns of the table will be copied.

### *query*

Specify a `SELECT`, `VALUES`, `INSERT`, `UPDATE`, or `DELETE` statement whose results are to be copied. For `INSERT`, `UPDATE`, and `DELETE` statements, a RETURNING clause must be provided.

### *filename*

Specify the path of the file to be copied. An input file name can be an absolute or relative path, but an output file name must be an absolute path. Critically, the file must be located _server-side_ on the local filesystem of the YB-TServer that you connect to.

To work with files that reside on the client, nominate `stdin` as the argument for `FROM` or `stdout` as the argument for `TO`.  

Alternatively, you can use the `\copy` metacommand in [`ysqlsh`](../../../../../admin/ysqlsh#copy-table-column-list-query-from-to-filename-program-command-stdin-stdout-pstdin-pstdout-with-option).

### *stdin* and *stdout*

Critically, these input and output channels are defined _client-side_ in the environment of the client where you run  [`ysqlsh`](../../../../../admin/ysqlsh#copy-table-column-list-query-from-to-filename-program-command-stdin-stdout-pstdin-pstdout-with-option) or your preferred programming language. These options request that the data transmission goes via the connection between the client and the server.

If you execute the `COPY TO` or `COPY FROM` statements  from a client program written in a language like Python, then you cannot use ysqlsh features. Rather, you must rely on your chosen language's features to connect `stdin` and `stdout` to the file that you nominate.

However, if  you execute `COPY FROM` using  [`ysqlsh`](../../../../../admin/ysqlsh#copy-table-column-list-query-from-to-filename-program-command-stdin-stdout-pstdin-pstdout-with-option), you have the further option of including the `COPY` invocation at the start of the file that you start as a `.sql` script. Create a test table thus:

```plpgsql
drop table if exists t cascade;
create table t(c1 text primary key, c2 text, c3 text);
```


And prepare `t.sql` thus:

```
copy t(c1, c2, c3) from stdin with (format 'csv', header true);
c1,c2,c3
dog,cat,frog
\.
```
Notice the `\.` terminator. You can simply execute `\i t.sql` at the  [`ysqlsh`](../../../../../admin/ysqlsh#copy-table-column-list-query-from-to-filename-program-command-stdin-stdout-pstdin-pstdout-with-option) prompt to copy in the data.

{{< note title="Some client-side languages have a dedicated exposure of COPY" >}}

For example, the _"psycopg2"_ PostgreSQL driver for Python (and of course this works for YugabyteDB) has dedicated cursor methods for `COPY`.  See <a href="https://www.psycopg.org/docs/usage.html#using-copy-to-and-copy-from" target="_blank">Using COPY TO and COPY FROM <i class="fas fa-external-link-alt"></i></a>

{{< /note >}}

## Examples

The examples below assume a table like this:

```plpgsql
yugabyte=# CREATE TABLE users(id BIGSERIAL PRIMARY KEY, name TEXT);
yugabyte=# INSERT INTO users(name) VALUES ('John Doe'), ('Jane Doe'), ('Dorian Gray');
yugabyte=# SELECT * FROM users;
 id |    name
----+-------------
  3 | Dorian Gray
  2 | Jane Doe
  1 | John Doe
(3 rows)
```

### Export an entire table

Copy the entire table to a CSV file using an absolute path, with column names in the header.

```plpgsql
yugabyte=# COPY users TO '/home/yuga/Desktop/users.txt.sql' DELIMITER ',' CSV HEADER;
```

### Export a partial table using the WHERE clause with column selection

In the following example, a `WHERE` clause is used to filter the rows and only the `name` column.


```plpgsql
yugabyte=# COPY (SELECT name FROM users where name='Dorian Gray') TO '/home/yuga/Desktop/users.txt.sql' DELIMITER
 ',' CSV HEADER;
```

### Import from CSV files

In the following example, the data exported in the previous examples are imported in the `users` table.

```plpgsql
yugabyte=# COPY users FROM '/home/yuga/Desktop/users.txt.sql' DELIMITER ',' CSV HEADER;
```


### Import a large table using smaller transactions

When importing a very large table, Yugabyte recommends using many smaller transactions (rather than one large transaction).
This can be achieved natively by using the `ROWS_PER_TRANSACTION` option.

```plpgsql
yugabyte=# COPY large_table FROM '/home/yuga/Desktop/large_table.csv'
               WITH (FORMAT CSV, HEADER, ROWS_PER_TRANSACTION 1000);
```


- If the table does not exist, errors are raised.
- `COPY TO` can only be used with regular tables.
- `COPY FROM` can be used with tables, foreign tables, and views.
