---
title: array_position() and array_positions()
linkTitle: array_position(), array_positions()
headerTitle: array_position() and array_positions()
description: array_position() and array_positions()
menu:
  latest:
    identifier: array-position
    parent: array-functions-operators
isTocNested: true
showAsideToc: true
---
These functions require that the to-be-searched array is one-dimensional. They return the index values of the specified to-be-searched-for value in the specified to-be-searched array.

Create _"view v"_ now. The examples below use it.
```plpgsql
create or replace view v as
select array[
    'sun', -- 1
    'mon', -- 2
    'tue', -- 3
    'wed', -- 4
    'thu', -- 5
    'fri', -- 6
    'sat', -- 7
    'mon'  -- 9
  ]::text
as arr;
```

## array_position()

**Purpose:** Return the index, in the supplied array, of the specified value. Optionally, starts searching at the specified index.

**Signature:**
```
input value:       anyarray, anyelement [, int]
return value:      int
```
**Note:** The optional third parameter specifies the _inclusive_ index value at which to start the search.

**Example:**
```plpgsql
select array_position(
  (select arr from v)::text[],  -- #1. The to-be-searched array.

  'mon'::text,                  -- #2. The to-be-searched-for value.

  3::int                        -- #3. The (inclusive) position at which to start searching. [optional]
) as position;
```
This is the result:

```
 position 
----------
        8
```

## array_positions()

**Purpose:** Return the indexes, in the supplied array, of all occurrences of the specified value.

**Signature:**

```
input value:       anyarray, anyelement
return value:      integer[] 
```
**Example:**
```plpgsql
select array_positions(
  (select arr from v)::text[],  -- #1. The to-be-searched array.

  'mon'::text                   -- #2. The to-be-searched-for value.
) as positions;
```
This is the result:

```
 positions 
-----------
 {2,8}
```
