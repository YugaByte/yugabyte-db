---
title: jsonb_set() and jsonb_insert()
headerTitle: jsonb_set() and jsonb_insert()
linkTitle: jsonb_set() and jsonb_insert()
description: Change a JSON value using jsonb_set() and insert a value using jsonb_insert().
menu:
  latest:
    identifier: jsonb-set-jsonb-insert
    parent: json-functions-operators
    weight: 210
isTocNested: true
showAsideToc: true
---

**Purpose**: Use `jsonb_set()` to change a JSON value that is the value of an existing key-value pair in a JSON _object_ or the value at an existing index in a JSON array. Use `jsonb_insert()` to insert a value, either as the value for a key that doesn't yet exist in a JSON _object_ or beyond the end or before the start of the index range, for a JSON _array_.

**Signature:** For `jsonb_set()`:

```
jsonb_in:           jsonb
path:               text[]
replacement:        jsonb
create_if_missing:  boolean default true
return value:       jsonb
```

**Signature:** For `jsonb_insert()`:

```
jsonb_in:           jsonb
path:               text[]
replacement:        jsonb
insert_after:       boolean default false
return value:       jsonb
```
**Notes:**

- There is no `json` variant.

- It turns out that the effect of the two functions is the same in some cases. This brings useful "upsert" functionality when the target is a JSON _array_.

- The meaning of the defaulted `boolean` formal parameter is context dependent.

- The input JSON value must be either an _object_ or an _array_—in other words, it must have elements that can be addressed by a path.

## Semantics when "jsonb&#95;in" is a JSON object
parsed, or when two JSON values are concatenated, and if a key is repeated, then the last-mentioned in left-to-right order wins.) The functionality is sufficiently illustrated by a _"json_in"_ value that has just primitive values. The result of each function invocation is the same.

```plpgsql
do $body$
declare
  j constant jsonb := '{"a": 1, "b": 2, "c": 3}';
  path constant text[] := array['d'];
  new_number constant jsonb := '4';

  j_set constant jsonb := jsonb_set(
    jsonb_in          => j,
    path              => path,
    replacement       => new_number,
    create_if_missing => true);

  j_insert constant jsonb := jsonb_insert(
    jsonb_in          => j,
    path              => path,
    replacement       => new_number,
    insert_after      => false);

  expected_j constant jsonb := '{"a": 1, "b": 2, "c": 3, "d": 4}';

begin
  assert
    j_set = expected_j and
    j_insert = expected_j,
  'unexpected';
end;
$body$;
```

Notice that the specified `path`, the key `"d"` doesn't yet exist. Each function call asks to produce the result that the key `"d"` should exist with  the value `4`. So, as you see, the effect of each, as written above, is the same.

If `jsonb_set()` is invoked with _"create_if_missing"_ set to `FALSE`, then its result is the same as the input. But if `jsonb_insert()` is invoked with _"insert_after"_ set to `TRUE`, then its output is the same as when it's invoked with _"insert_after"_ set to `FALSE`. This reflects the fact that the order of key-value pairs in an _object_ is insignificant.

What if _"path"_ specifies a key that does already exist? Now `jsonb_insert()` causes this error when it's invoked both with _"insert_after"_ set to `TRUE` and with _"insert_after"_ set to `FALSE`:
```
cannot replace existing key
Try using the function jsonb_set to replace key value.
```

And this `DO` block quietly succeeds, both when it's invoked with _"create_if_missing"_ set to `FALSE` and when it's invoked with _"create_if_missing"_ set to `TRUE`.

```plpgsql
do $body$
declare
  j constant jsonb := '{"a": 1, "b": 2, "c": 3}';
  path constant text[] := array['c'];
  new_number constant jsonb := '4';

  j_set constant jsonb := jsonb_set(
    jsonb_in          => j,
    path              => path,
    replacement       => new_number,
    create_if_missing => true);

  expected_j constant jsonb := '{"a": 1, "b": 2, "c": 4}';

begin
  assert
    j_set = expected_j,
  'unexpected';
end;
$body$;
```
## Semantics when "jsonb&#95;in" is an JSON array

A JSON _array_ is a list of index-addressable values—in other words, the order is defined and significant. Again, the functionality is sufficiently illustrated by a `json_in` value that has just primitive values. Now the result of `jsonb_set()` differs from that of `jsonb_insert()`. 

```plpgsql
do $body$
declare
  j constant jsonb := '["a", "b", "c", "d"]';
  path constant text[] := array['3'];
  new_string constant jsonb := '"x"';

  j_set constant jsonb := jsonb_set(
    jsonb_in          => j,
    path              => path,
    replacement       => new_string,
    create_if_missing => true);

  j_insert constant jsonb := jsonb_insert(
    jsonb_in     => j,
    path         => path,
    replacement  => new_string,
    insert_after => true);

  expected_j_set    constant jsonb := '["a", "b", "c", "x"]';
  expected_j_insert constant jsonb := '["a", "b", "c", "d", "x"]';

begin
  assert
    (j_set = expected_j_set) and
    (j_insert = expected_j_insert),
  'unexpected';
end;
$body$;
```

Notice that the path denotes the fourth value and that this already exists.

Here, `jsonb_set()` located the fourth value and set it to _"x"_ while `jsonb_insert()` located the fourth value and, as requested by _"insert_after"_ set to `TRUE`, inserted _"x"_ after it. Of course, with _"insert_after"_ set to `FALSE`, _"x"_ is inserted before _"d"_. And (of course, again) the choice for _"create_if_missing"_ has no effect on the result of `jsonb_set()`.

What if the path denotes a value beyond the end of the array?

```plpgsql
do $body$
declare
  j constant jsonb := '["a", "b", "c", "d"]';
  path constant text[] := array['42'];
  new_string constant jsonb := '"x"';

  j_set constant jsonb := jsonb_set(
    jsonb_in          => j,
    path              => path,
    replacement       => new_string,
    create_if_missing => true);

  j_insert constant jsonb := jsonb_insert(
    jsonb_in          => j,
    path              => path,
    replacement       => new_string,
    insert_after      => true);

  expected_j constant jsonb := '["a", "b", "c", "d", "x"]';

begin
  assert
    j_set = expected_j and
    j_insert = expected_j,
  'unexpected';
end;
$body$;
```

Here, each function had the same effect.

The path, for `jsonb_set()`, is taken to mean the as yet nonexistent fifth value. So, with _"create_if_missing"_ set to `FALSE`, `jsonb_set()` has no effect.

The path, for `jsonb_insert()`, is also taken to mean the as yet nonexistent fifth value. But now, the choice of `TRUE` or `FALSE` for _"insert_after"_ makes no difference because before, or after, a nonexistent element is taken to mean insert it.

Notice that if the path is specified as `-42` (i.e. an impossible _array_ index) the result is to establish the specified value at the _start_ of the _array_. `jsonb_set` and `jsonb_insert` produce the same result, this:

```plpgsql
do $body$
declare
  j constant jsonb := '["a", "b", "c", "d"]';
  path constant text[] := array['-42'];
  new_string constant jsonb := '"x"';

  j_set constant jsonb := jsonb_set(
    jsonb_in          => j,
    path              => path,
    replacement       => new_string,
    create_if_missing => true);

  j_insert constant jsonb := jsonb_insert(
    jsonb_in          => j,
    path              => path,
    replacement       => new_string,
    insert_after      => true);

  expected_j constant jsonb := '["x", "a", "b", "c", "d"]';

begin
  assert
    j_set = expected_j and
    j_insert = expected_j,
  'unexpected';
end;
$body$;
```

The path, for `jsonb_set()`, is taken to mean a new first value (implying that the existing values all move along one place). So, again, with _"create_if_missing"_ set to `FALSE`, `jsonb_set()` has no effect. 

The path, for `jsonb_insert()`, is also taken to mean a new first value. So again, the choice of `TRUE` or `FALSE` for _"insert_after"_ makes no difference because before or after, a nonexistent element is taken to mean insert it.
