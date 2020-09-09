---
title: ECHO
linkTitle: ECHO
description: ECHO
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2050
aliases:
  - /stable/api/redis/echo
  - /stable/api/yedis/del
isTocNested: true
showAsideToc: true
---

## Synopsis
<b>`ECHO message`</b><br>
This command outputs the given `message`.

## Return Value
Returns the message.

## Examples

You can do this as shown below.

```sh
$ ECHO "Yuga Yuga"
```

```
"Yuga Yuga"
```
