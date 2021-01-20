---
title: JSONB data type [YCQL]
headerTitle: JSONB
linkTitle: JSONB
description: Use the JSONB data type to efficiently model json data. This data type makes it easy to model JSON data which does not have a set schema and might change often.
menu:
  latest:
    parent: api-cassandra
    weight: 1470
aliases:
  - /latest/api/cassandra/type_jsonb
  - /latest/api/ycql/type_jsonb
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `JSONB` data type to efficiently model JSON data. This data type makes it easy to model
JSON data which does not have a set schema and might change often. This data type is similar to
the [JSONB data type in PostgreSQL](https://www.postgresql.org/docs/9.4/static/datatype-json.html).
The JSON document is serialized into a format which is easy for search and retrieval.
This is achieved by storing all the JSON keys in sorted order, which allows for efficient binary
search of keys. Similarly, arrays are stored such that random access for a particular array index
into the serialized json document is possible.

Currently, updates to some attributes of a JSONB column require a full read-modify-write operation.
Note that there are plans to enhance the JSONB data type to support efficient incremental updates in
a future version.

## Syntax

```
type_specification ::= { JSONB }
```

## Semantics

- Columns of type `JSONB` cannot be part of the `PRIMARY KEY`.
- Implicitly, values of type `JSONB` are not convertible to other data types. `JSONB` types can be
  compared to `TEXT/VARCHAR` data type as long it represents valid json.
- Values of text data types with correct format are convertible to `JSONB`.
- `JSONB` value format supports text literals which are valid json.

{{< note title="Note" >}}

Internally, numbers that appear in a JSONB string (used without quotes. e.g `{'a': 3.14}` ) are stored as floating point values. 
Due to the inherent imprecision in storing floating-point numbers, one should avoid comparing them for equality. 
Users can either use error bounds while querying for these values in order to perform the correct floating-point comparison, or store them as strings (e.g: `{'a': "3.14"}`). 
[#996 issue](https://github.com/yugabyte/yugabyte-db/issues/996)


{{< /note >}}


## Operators and functions

We currently support two operators which can be applied to the `JSONB` data type. The `->` operator 
returns a result of type `JSONB` and further json operations can be applied to the result. The `->>` 
operator converts `JSONB` to its string representation and returns the same. As a result, you can't
apply further `JSONB` operators to the result of the `->>` operator. These operators can either have
a string (for keys in a json object) or integer (for array indices in a json array) as a parameter.

In some cases, you would like to process JSON attributes as numerics. For this purpose, you can use
the `CAST` function to convert text retrieved from the `->>` operator to the appropriate numeric
type.

## Examples

- Create table with a JSONB column

```sql
ycqlsh> CREATE KEYSPACE store;
```

```sql
ycqlsh> CREATE TABLE store.books ( id int PRIMARY KEY, details jsonb );
```

- Insert JSONB documents

```sql
INSERT INTO store.books (id, details) VALUES
  (1, '{ "name": "Macbeth", "author": { "first_name": "William", "last_name": "Shakespeare" }, "year": 1623, "editors": ["John", "Elizabeth", "Jeff"] }');
INSERT INTO store.books (id, details) VALUES 
  (2, '{ "name": "Hamlet", "author": { "first_name": "William", "last_name": "Shakespeare" }, "year": 1603, "editors": ["Lysa", "Mark", "Robert"] }');
INSERT INTO store.books (id, details) VALUES 
  (3, '{ "name": "Oliver Twist", "author": { "first_name": "Charles", "last_name": "Dickens" }, "year": 1838, "genre": "novel", "editors": ["Mark", "Tony", "Britney"] }');
INSERT INTO store.books (id, details) VALUES 
  (4, '{ "name": "Great Expectations", "author": { "first_name": "Charles", "last_name": "Dickens" }, "year": 1950, "genre": "novel", "editors": ["Robert", "John", "Melisa"] }');
INSERT INTO store.books (id, details) VALUES 
  (5, '{ "name": "A Brief History of Time", "author": { "first_name": "Stephen", "last_name": "Hawking" }, "year": 1988, "genre": "science", "editors": ["Melisa", "Mark", "John"] }');
```

- Select from JSONB column

```sql
ycqlsh> SELECT * FROM store.books;
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------------------
  5 | {"author":{"first_name":"Stephen","last_name":"Hawking"},"editors":["Melisa","Mark","John"],"genre":"science","name":"A Brief History of Time","year":1988}
  1 |                            {"author":{"first_name":"William","last_name":"Shakespeare"},"editors":["John","Elizabeth","Jeff"],"name":"Macbeth","year":1623}
  4 |      {"author":{"first_name":"Charles","last_name":"Dickens"},"editors":["Robert","John","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
  2 |                                {"author":{"first_name":"William","last_name":"Shakespeare"},"editors":["Lysa","Mark","Robert"],"name":"Hamlet","year":1603}
  3 |             {"author":{"first_name":"Charles","last_name":"Dickens"},"editors":["Mark","Tony","Britney"],"genre":"novel","name":"Oliver Twist","year":1838}
```

- Select with condition on JSONB object value

```sql
ycqlsh> SELECT * FROM store.books WHERE details->'author'->>'first_name' = 'William' AND details->'author'->>'last_name' = 'Shakespeare';
```

```
 id | details
----+----------------------------------------------------------------------------------------------------------------------------------
  1 | {"author":{"first_name":"William","last_name":"Shakespeare"},"editors":["John","Elizabeth","Jeff"],"name":"Macbeth","year":1623}
  2 |     {"author":{"first_name":"William","last_name":"Shakespeare"},"editors":["Lysa","Mark","Robert"],"name":"Hamlet","year":1603}
```

- Select with condition on JSONB array element

```sql
ycqlsh> SELECT * FROM store.books WHERE details->'editors'->>0 = 'Mark';
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------
  3 | {"author":{"first_name":"Charles","last_name":"Dickens"},"editors":["Mark","Tony","Britney"],"genre":"novel","name":"Oliver Twist","year":1838}
```

- Select with condition using on JSONB element

```sql
ycqlsh> SELECT * FROM store.books WHERE CAST(details->>'year' AS integer) = 1950;
```

```
 id | details
----+--------------------------------------------------------------------------------------------------------------------------------------------------------
  4 | {"author":{"first_name":"Charles","last_name":"Dickens"},"editors":["Robert","John","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
```

- Update entire JSONB document

You can do this as shown below.

```sql
ycqlsh> UPDATE store.books SET details = '{"author":{"first_name":"Carl","last_name":"Sagan"},"editors":["Ann","Rob","Neil"],"genre":"science","name":"Cosmos","year":1980}' WHERE id = 1;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 1;
```

```
 id | details
----+-----------------------------------------------------------------------------------------------------------------------------------
  1 | {"author":{"first_name":"Carl","last_name":"Sagan"},"editors":["Ann","Rob","Neil"],"genre":"science","name":"Cosmos","year":1980}
```

- Update a JSONB object value.

```sql
ycqlsh> UPDATE store.books SET details->'author'->>'first_name' = '"Steve"' WHERE id = 4;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 4;
```

```
 id | details
----+------------------------------------------------------------------------------------------------------------------------------------------------------
  4 | {"author":{"first_name":"Steve","last_name":"Dickens"},"editors":["Robert","John","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
```

- Update a JSONB array element.

```sql
ycqlsh> UPDATE store.books SET details->'editors'->>1 = '"Jack"' WHERE id = 4;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 4;
```

```
 id | details
----+------------------------------------------------------------------------------------------------------------------------------------------------------
  4 | {"author":{"first_name":"Steve","last_name":"Dickens"},"editors":["Robert","Jack","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
```

- Update a JSONB subdocument.

```sql
ycqlsh> UPDATE store.books SET details->'author' = '{"first_name":"John", "last_name":"Doe"}' WHERE id = 4;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 4;
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------
  4 | {"author":{"first_name":"John","last_name":"Doe"},"editors":["Robert","Jack","Melisa"],"genre":"novel","name":"Great Expectations","year":1950}
```

- Upsert: Update a missing JSONB document resulting in an insert.

```sql
INSERT INTO store.books (id, details) VALUES
  (6, '{}');
ycqlsh> UPDATE store.books SET details->'editors' = '["Adam", "Bryan", "Charles"]' WHERE id = 6;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 6;
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------
  6 | {"editors":["Adam","Bryan","Charles"]}
```

- Upsert: Update a missing JSONB document resulting in an insert of a subdocument.

```sql
ycqlsh> UPDATE store.books SET details->'author' = '{"first_name":"Jack", "last_name":"Kerouac"}' WHERE id = 6;
```

```sql
ycqlsh> SELECT * FROM store.books WHERE id = 6;
```

```
 id | details
----+-------------------------------------------------------------------------------------------------------------------------------------------------
  6 | {"author":{"first_name":"Jack","last_name":"Kerouac"},"editors":["Adam","Bryan","Charles"]}
```

Note that JSONB upsert only works for JSON objects and not for other data types like arrays, integers, strings, etc. Additionally, only the leaf property of an object will be inserted if it is missing. We do not support upsert on non-leaf properties.

## See also

- [`Explore JSON documents`](../../../explore/transactional/json-documents)
- [Data types](..#data-types)
