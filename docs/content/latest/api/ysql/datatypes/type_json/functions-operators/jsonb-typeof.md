---
title: jsonb_typeof() and json_typeof()
headerTitle: jsonb_typeof() and json_typeof()
linkTitle: jsonb_typeof()
description: Return the data type of the JSON value as a SQL text value.
menu:
  latest:
    identifier: jsonb-typeof
    parent: json-functions-operators
    weight: 250
isTocNested: true
showAsideToc: true
---

**Purpose:** Return the data type of the JSON value as a SQL `text` value.

**Signature** For the `jsonb` variant:

```
input value:       jsonb
return value:      text
```

**Notes:** Possible return values are _string_, _number_, _boolean_, _null_,  _object_, and _array_, as follows.

```plpgsql
do $body$
declare
  j_string   constant jsonb := '"dog"';
  j_number   constant jsonb := '42';
  j_boolean  constant jsonb := 'true';
  j_null     constant jsonb := 'null';
  j_object   constant jsonb := '{"a": 17, "b": "x", "c": true}';
  j_array    constant jsonb := '[17, "x", true]';
begin
  assert
    jsonb_typeof(j_string)   = 'string'  and
    jsonb_typeof(j_number)   = 'number'  and
    jsonb_typeof(j_boolean)  = 'boolean' and
    jsonb_typeof(j_null)     = 'null'    and
    jsonb_typeof(j_object)   = 'object'  and
    jsonb_typeof(j_array)    = 'array',
 'unexpected';
end;
$body$;
```
