---
title: array_remove()
linkTitle: array_remove()
headerTitle: array_remove()
description: array_remove()
menu:
  latest:
    identifier: array-remove
    parent: array-functions-operators
isTocNested: true
showAsideToc: true
---

**Purpose:** Return a new array where _every_ occurrence of the specified value has been removed from the specified input array.

**Signature:**
```
input value:       anyarray, anyelement
return value:      anyarray
```
**Note:** This function requires the array from which values are to be removed is one-dimensional. This restriction is understood in light of the fact that arrays are rectilinear—in other words, the geometry of an array whose dimensionality is two or more is fixed at creation time. For examples illustrating this rule, see [`array_fill()`](.././array-fill).

**Example:**
```plpgsql
create table t(k int primary key, arr int[]);
insert into t(k, arr)
values (1, '{1, 2, 2, 2, 5, 6}'::int[]);

select arr as "old value of arr" from t where k = 1;

update t
set arr = array_remove(arr, 2)
where k = 1;

select arr as "new value of arr" from t where k = 1;
```
This is the result of the two queries:
```
 old value of arr 
------------------
 {1,2,2,2,5,6}

 new value of arr 
------------------
 {1,5,6}
```
