---
title: Data modeling in YCQL
headerTitle: Data modeling
linkTitle: 2. Data modeling
description: Learn data modeling in YCQL and how to identify the patterns used to access data and the types of queries to be performed.
block_indexing: true
menu:
  v2.1:
    identifier: data-modeling-1-ycql
    parent: learn
    weight: 562
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/develop/learn/data-modeling-ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/develop/learn/data-modeling-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

Data modeling is a process that involves identifying the entities (items to be stored) and the relationships between entities. To create your data model, identify the patterns used to access data and the types of queries to be performed. These two ideas inform the organization and structure of the data, and the design and creation of the database's tables.

This topic documents data modeling with [Yugabyte Cloud Query Language (YCQL)](../../../api/ycql), YugabyteDB's Cassandra-compatible API.

## Keyspaces, tables, rows, and columns

### Keyspaces

Cassandra keyspaces are a collection of tables. They are analogous to SQL namespaces. Typically, each application creates all its tables in one keyspace.

### Tables

A table is a collection of data. A keyspace most often contains one or more tables. Each table is identified by a name. Tables have a set of columns and contain records (rows) with data. Tables can be created, dropped, and altered at runtime without blocking updates and queries.

### Rows

Each table contains multiple rows of data. A row is a set of columns that is uniquely identifiable among all of the other rows.

### Columns

Each row is composed of one or more columns. A column is a fundamental data element, and does not need to be broken down any further.

As the example of a `users` table which holds information about users of a service.

| user_id  | firstname | lastname | address
| -------- | --------- | -------- | -------- 
| 1001     | Sherlock  | Holmes   | 221b Baker St, London, UK
| 1003     | Clark     | Kent     | 344 Clinton Street, Metropolis
| 1007     | James     | Bond     |

Note the following about the `users` table:

- Each row in the table has a unique value for the primary key column (`user_id`).
- Other than the primary key, the `users` table has three other columns - `firstname`, `lastname`, `address` each of which is a string.
- Some columns may have no data (for example, James Bond's address `address` is unknown). These have `null` values in the database.

Now consider another example of the `books` table that keeps track of authors and the books they have written.

| author               | book_title           | price  | year | genre
| -------------------- | -------------------- | ------ | ---- | ----- 
| William Shakespeare  | Hamlet               | 6.75   | 1602 | tragedy
| William Shakespeare  | Macbeth              | 7.50   | 1606 | tragedy
| Charles Dickens      | Oliver Twist         | 9.25   | 1837 | serial novel
| Charles Dickens      | A Tale of Two Cities | 11.40  | 1859 | historical novel

Note the following about the `books` table:

- The primary key for this table consists of two columns - `author` and `book_title`. Each row in the table must have values for these two attributes, and the combination of these values must be unique.
- Other than the primary key, the table has other columns such as `price`, `year`, `genre`.
- The columns `author`, `book_title` and `genre` are string, `price` is a float, `year` is an integer.

## Primary key

When creating a table, the primary key of the table must be specified in addition to the table name. The primary key uniquely identifies each row in the table, therefore no two rows can have the same key.

There are two types of primary keys, and they are described below.

### Partition key columns (required)

Such tables have *simple primary keys*. One or more columns of a table can be made the partition key columns. The values of the partition key columns are used to compute an internal hash value. This hash value determines the tablet (or partition) in which the row will be stored. This has two implications:

- Each unique set of partition key values is hashed and distributed across nodes randomly to ensure uniform utilization of the cluster.

- All the data for a unique set of partition key values are always stored on the same node. This matters only if there are clustering key columns, which are described in the next section.

In the case of the `users` table, you can make `user_id` column the only primary key column. This is a good choice for a partition key because our queries do not care about the order of the `user_id`s. If the table is split into a number of tablets (partitions), the data may be assigned as follows.

| tablet    | user_id  | firstname | lastname | address
| --------- | -------- | --------- | -------- | -------- 
| tablet-22 | 1001     | Sherlock  | Holmes   | 221b Baker St, London, UK
| tablet-4  | 1003     | Clark     | Kent     | 344 Clinton Street, Metropolis
| tablet-17 | 1007     | James     | Bond     |

### Clustering key columns (optional)

The clustering columns specify the order in which the column data is sorted and stored on disk for a given unique partition key value. More than one clustering column can be specified, and the columns are sorted in the order they are declared in the clustering column. It is also possible to control the sort order (ascending or descending sort) for these columns. Note that the sort order respects the data type.

In a table that has both partition keys and clustering keys, it is possible for two rows to have the same partition key value and therefore they end up on the same node. However, those rows must have different clustering key values in order to satisfy the primary key requirements.

In the case of the `books` table, `author` is a good partition key and `book_title` is a good clustering key. Such a data model would allow easily listing all the books for a given author, as well as look up details of a specific book. This would cause the data to be stored as follows.

| tablet    | author               | book_title           | price  | year | genre
| --------- | -------------------- | -------------------- | ------ | ---- | ----- 
| tablet-15 | William Shakespeare  | Hamlet               | 6.75   | 1602 | tragedy
| tablet-15 | William Shakespeare  | Macbeth              | 7.50   | 1606 | tragedy
| tablet-21 | Charles Dickens      | A Tale of Two Cities | 11.40  | 1859 | historical novel
| tablet-21 | Charles Dickens      | Oliver Twist         | 9.25   | 1837 | serial novel

Note that if you had made both `author` and `book_title` partition key columns, you would not be able to list all the books for a given author efficiently.

**Note**

- The partition key columns are also often referred to as its  *hash columns*. This is because an internal hash function is used to distribute data items across tablets based on their partition key values.

- The clustering key columns are also referred to as its **range columns**. This is because rows with the same partition key are stored on disk in sorted order by the clustering key value.

## Secondary indexes

A database index is a data structure that improves the speed of data retrieval operations on a database table. Typically, databases are very efficient at looking up data by the primary key. A secondary index can be created using one or more columns of a database table, and provides the basis for both rapid random lookups and efficient access of ordered records when querying by those columns. To achieve this, secondary indexes require additional writes and storage space to maintain the index data structure. YugabyteDB's secondary index support is documented in detail [here](../../../api/ycql/ddl_create_index/).

### Benefits of secondary indexes

Secondary indexes can be used to speed up queries and to enforce uniqueness of values in a column.

#### Speed up queries

The predominant use of a secondary index is to make lookups by some column values efficient. Let us take an example of a users table, where `user_id` is the primary key. Suppose we want to lookup `user_id` by the email of the user efficiently. You can achieve this as follows.

```sql
ycqlsh> CREATE KEYSPACE example;
```

```sql
ycqlsh> CREATE TABLE example.users(
         user_id    bigint PRIMARY KEY,
         firstname  text,
         lastname   text,
         email      text
       ) WITH transactions = { 'enabled' : true };
```

```sql
ycqlsh> CREATE INDEX user_by_email ON example.users (email)
         INCLUDE (firstname, lastname);
```

Next let us insert some data.

```sql
ycqlsh> INSERT INTO example.users (user_id, firstname, lastname, email) 
       VALUES (1, 'James', 'Bond', 'bond@yb.com');
```

```sql
ycqlsh> INSERT INTO example.users (user_id, firstname, lastname, email) 
       VALUES (2, 'Sherlock', 'Holmes', 'sholmes@yb.com');
```

You can now query the table by the email of a user efficiently as follows.

```sql
ycqlsh> SELECT * FROM example.users WHERE email='bond@yb.com';
```

Read more about using secondary indexes to speed up queries in this quick guide to YugabyteDB secondary indexes.

### Enforce uniqueness of column values

In some cases, you would need to ensure that duplicate values cannot be inserted in a column of a table. You can achieve this in YugabyteDB by creating a unique secondary index, where the application does not want duplicate values to be inserted into a column.

```sql
ycqlsh> CREATE KEYSPACE example;
```

```sql
ycqlsh> CREATE TABLE example.users(
         user_id    bigint PRIMARY KEY,
         firstname  text,
         lastname   text,
         email      text
       ) WITH transactions = { 'enabled' : true };
```

```sql
ycqlsh> CREATE UNIQUE INDEX unique_emails ON example.users (email);
```

Inserts would succeed as long as the email is unique.

```sql
ycqlsh> INSERT INTO example.users (user_id, firstname, lastname, email) 
       VALUES (1, 'James', 'Bond', 'bond@yb.com');
```

```sql
ycqlsh> INSERT INTO example.users (user_id, firstname, lastname, email) 
       VALUES (2, 'Sherlock', 'Holmes', 'sholmes@yb.com');
```

But upon inserting a duplicate email, you get an error.

```sql
ycqlsh> INSERT INTO example.users (user_id, firstname, lastname, email) 
       VALUES (3, 'Fake', 'Bond', 'bond@yb.com');
```

```
InvalidRequest: Error from server: code=2200 [Invalid query] message="SQL error: Execution Error. Duplicate value disallowed by unique index unique_emails
```

## Documents

Documents are the most common way for storing, retrieving, and managing semi-structured data. Unlike the traditional relational data model, the document data model is not restricted to a rigid schema of rows and columns. The schema can be changed easily  thus helping application developers write business logic faster than ever before. Instead of columns with names and data types that are used in a relational model, a document contains a description of the data type and the value for that description. Each document can have the same or different structure. Even nested document structures are possible where one or more sub-documents are embedded inside a larger document.

Databases commonly support document data management through the use of a JSON data type. [JSON.org](http://www.json.org/) defines JSON (JavaScript Object Notation) to be a lightweight data-interchange format. It’s easy for humans to read and write. it’s easy for machines to parse and generate. JSON has four simple data types:

- string
- number
- boolean
- null (or empty)

In addition, it has two core complex data types.

- Collection of name-value pairs which is realized as an object, hash table, dictionary, or something similar depending on the language.
- Ordered list of values which is realized as an array, vector, list or sequence depending on the language.

Document data models are best fit for applications requiring a flexible schema and fast data access. For example, nested documents enable applications to store related pieces of information in the same database record in a denormalized manner. As a result, applications can issue fewer queries and updates to complete common operations.

### Comparison with Apache Cassandra’s JSON support

[Apache Cassandra’s JSON](http://cassandra.apache.org/doc/latest/cql/json.html) support can be misleading for many developers. YCQL allows `SELECT` and `INSERT` statements to include the `JSON` keyword. The `SELECT` output will now be available in the JSON format and the `INSERT` inputs can now be specified in the JSON format. However, this “JSON” support is simply an ease-of-use abstraction in the CQL layer that the underlying database engine is unaware of. Since there is no native JSON data type in CQL, the schema doesn’t have any knowledge of the JSON provided by the user. This means the schema definition doesn’t change nor does the schema enforcement. Cassandra developers needing native JSON support previously had no choice but to add a new document database such as MongoDB or Couchbase into their data tier.

With YugabyteDB’s native JSON support using the [`JSONB`](../data-types/#jsonb) data type, application developers can now benefit from the structured query language of Cassandra and the document data modeling of MongoDB in a single database.
