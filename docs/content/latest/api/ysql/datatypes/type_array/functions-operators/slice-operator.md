---
title: The array slice operator
linkTitle: Array slice operator 
headerTitle: The array slice operator
description: The array slice operator
menu:
  latest:
    identifier: array-slice-operator
    parent: array-functions-operators
    weight: 30
isTocNested: true
showAsideToc: true
---

**Purpose:** Return a new array whose length is defined by specifying the slice's lower and upper bound along each dimension.

**Signature:**
```
input value:       [lb_1:ub_1] ... [lb_N:ub_N]anyarray
return value:      anyarray
```
**Note:**
- You must specify the lower and upper slicing bounds, as `int` values for each of the input array's N dimensions.
- The specified slicing bounds must not exceed the source array's bounds.
- The new array has the same dimensionality as the source array and its lower bound is `1` on each axis.

**Example:**
```plpgsql
create table t(k int primary key, arr text[]);

insert into t(k, arr)
values (1, '
  [2:4][3:6][4:5]=
  {
    {
      {a,b}, {c,d}, {e,f}, {g,h}
    },
    {
      {i,j}, {k,l}, {m,n}, {o,p}
    },
    {
      {q,r}, {s,t}, {u,v}, {w,x}
    }
  }
  '::text[]);

select arr as "old value of arr" from t where k = 1;

select
  array_lower(arr, 1) as "lb-1",
  array_upper(arr, 1) as "ub-1",
  array_lower(arr, 2) as "lb-2",
  array_upper(arr, 2) as "ub-2",
  array_lower(arr, 3) as "lb-3",
  array_upper(arr, 3) as "ub-3"
from t where k = 1;
```
It produces these results:
```
                                        old value of arr                                         
-------------------------------------------------------------------------------------------------
 [2:4][3:6][4:5]={{{a,b},{c,d},{e,f},{g,h}},{{i,j},{k,l},{m,n},{o,p}},{{q,r},{s,t},{u,v},{w,x}}}
```
and:
```
 lb-1 | ub-1 | lb-2 | ub-2 | lb-3 | ub-3 
------+------+------+------+------+------
    2 |    4 |    3 |    6 |    4 |    5
```
Now do the slicing:
```plpgsql
update t
set arr = arr[2:3][4:5][3:4]
where k = 1;

select arr as "new value of arr" from t where k = 1;

select
  array_lower(arr, 1) as "lb-1",
  array_upper(arr, 1) as "ub-1",
  array_lower(arr, 2) as "lb-2",
  array_upper(arr, 2) as "ub-2",
  array_lower(arr, 3) as "lb-3",
  array_upper(arr, 3) as "ub-3"
from t where k = 1;
```
It produces these results:
```
   new value of arr    
-----------------------
 {{{c},{e}},{{k},{m}}}
```
and:
```
 lb-1 | ub-1 | lb-2 | ub-2 | lb-3 | ub-3 
------+------+------+------+------+------
    1 |    2 |    1 |    2 |    1 |    1
```

Notice, that as promised, all the lower bounds are equal to `1`.
