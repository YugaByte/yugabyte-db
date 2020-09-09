---
title: Primitive and compound JSON data types
linkTitle: Primitive and compound data types
summary: Primitive and compound data types
headerTitle: Primitive and compound JSON data types
description: Learn how JSON can represent (sub)values of four primitive data types and of two compound data types.
block_indexing: true
menu:
  stable:
    identifier: primitive-and-compound-data-types
    parent: api-ysql-datatypes-json
    weight: 20
isTocNested: true
showAsideToc: true
---

JSON can represent (sub)values of four primitive data types and of two compound data types.

The primitive data types are _string_, _number_, _boolean_, and _null_. There is no way to declare the data type of a JSON value; rather, it emerges from the syntax of the representation.

Compare this with SQL and PL/pgSQL. SQL establishes the data type of a value from the metadata for the column in the table, or the field in the record, into which it is written or from which it is read. It also has the typecast notation, like `::text` or `::boolean` to establish the data type of a SQL literal. PL/pgSQL also supports the typecast notation and establishes the data type of a variable or a formal parameter by declaration. In this way, JSON is better compared with Python, which also implements an emergent data type paradigm. It is for this reason that, in the JSON type system, _null_ is defined as a data type rather than as a "value" (strictly, the absence of information about the value) of one of the other data types.

Notice that JSON cannot represent a date-time value except as a conventionally formatted _string_ value.

The two compound data types are _object_ and _array_.

A JSON literal is represented in a SQL statement or a PL/pgSQL program by the enquoted `::json` or `::jsonb` typecast of a `text` value that conforms to RFC 7159.

## JSON string

A JSON _string_ value is a sequence of zero, one, or many Unicode characters enclosed by the `"` character. Here are some examples, shown as SQL literals:

```
'"Dog"'::jsonb
```

The empty _string_ is legal, and is distinct from the JSON _null_.

```
'""'::jsonb
```

Case and whitespace are significant. Special characters within a _string_ value need to be escaped, thus:

- Backspace:&#160;&#160;&#160;_&#92;b_
- Form feed:&#160;&#160;&#160;_&#92;f_
- Newline:&#160;&#160;&#160;_&#92;n_
- Carriage return:&#160;&#160;&#160;_&#92;r_
- Tab:&#160;&#160;&#160;_&#92;t_
- Double quote:&#160;&#160;&#160;_&#92;&#34;_
- Backslash:&#160;&#160;&#160;_&#92;&#92;_

For example:

```
 '"\"First line\"\n\"second line\""'::jsonb
```

The explanation of the difference between the [`->` and `->>` operators](../functions-operators/subvalue-operators/) is illustrated nicely by this JSON _string_ value.

## JSON number

Here are some examples, shown as SQL literals:

```
'17'::jsonb
```

and:

```
'4.2'::jsonb
```

and:

```
'2.99792E8'::jsonb
```

Notice that JSON makes no distinction between integers and real numbers.

## JSON boolean

Here are the two allowed values, shown as SQL literals:

```
'true'::jsonb
```

and:

```
'false'::jsonb
```

## JSON null

As explained above, _null_ is special in JSON in that it is its own data type that allows exactly one "value", thus:

```
'null'::jsonb
```

## JSON object

An _object_ is a set of key-value pairs separated by _commas_ and surrounded by _curly braces_. The order is insignificant. The values in an _object_ do not have to have the same data types as each other. For example:

```
'{
  "a 1" : "Abc",
  "a 2" : 42,
  "a 3" : true,
  "a 4" : null,
  "a 5" : {"x" : 1, "y": "Pqr"}
}'::jsonb
```

Keys are case-sensitive and whitespace within such keys is significant. They can even contain characters that must be escaped. However, if a key does include spaces and special characters, the syntax that you need to read its value can become rather complex. It is sensible, therefore, to avoid exploiting this freedom.

An object can include more than one key-value pair with the same key. This is not recommended, but the outcome is well-defined: the last-mentioned key-value pair, in left-to-right order, in the set of key-value pairs with the same key "wins". You can test this by reading the value for a specified key with an operator like [`->`](../functions-operators/subvalue-operators/).

## JSON array

An _array_ is an ordered list of unnamed JSON values—in other words, the order is defined and is significant. The values in an _array_ do not have to have the same data types as each other. For example:

```
'[1, 2, "Abc", true, false, null, {"x": 17, "y": 42}]'::jsonb
```

The values in an array are indexed from `0`. See the account of the [`->`](../functions-operators/subvalue-operators/) operator.

## Example compound JSON value

```
{
  "given_name"         : "Fred",
  "family_name"        : "Smith",
  "email_address"      : "fred@yb.com",
  "hire_date"          : "17-Jan-2015",
  "job"                : "sales",
  "base_annual_salary" : 50000,
  "commisission_rate"  : 0.05,
  "phones"             : ["+11234567890", "+13216540987"]
}
```

This is a JSON _object_ with eight fields. The first seven are primitive _string_ or _number_ values (one of which conventionally represents a date) and the eighth is an array of two primitive _string_ values. The text representations of the phone numbers follow a convention by starting with + and  (presumably) a country code. JSON has no mechanisms for defining such conventions and for enforcing conformance.

{{< note title="Note" >}}

To see how these limitations can be ameliorated when a JSON document is stored in a column in a SQL table, see [Create indexes and check constraints on `json` and `jsonb` columns](../create-indexes-check-constraints/).

{{< /note >}}

In general, the top-level JSON document is an arbitrarily deep and wide hierarchy of subdocuments whose leaves are primitive values.

Notably, and in contrast to XML, JSON is not self-describing. Moreover, JSON does not support comments. But the intention is that the syntax should be reasonably intuitively obvious and human-readable.

It is the responsibility of the consumers of JSON documents to discover the composition rules of any corpus that they have to deal with by _ad hoc_ methods—at best external documentation and at worst human (or mechanical) inspection.

Most programming languages have data types that correspond directly to JSON's primitive data types and to its compound _object_ and _array_ data types.

{{< note title="Note" >}}

Because YSQL manipulates a JSON document as the value of a `json` or `jsonb` table-row intersection or PL/pgSQL variable, the terms "`json` [sub]value" or "`jsonb` [sub]value" (and JSON value as the superclass) are preferred from now on in this section—using "value" rather than "document".

{{< /note >}}
