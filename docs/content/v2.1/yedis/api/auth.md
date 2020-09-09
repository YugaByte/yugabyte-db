---
title: AUTH
linkTitle: AUTH
description: AUTH
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2020
isTocNested: true
showAsideToc: true
---
`AUTH` authenticates a client connection to Yugabyte YEDIS API.

## Synopsis

<b>`AUTH password`</b><br>
This command authenticates a client connection to YugabyteDB's YEDIS API.

YugabyteDB can be setup to require a password for clients to authenticate themselves. The password verification can be enforced by using the `CONFIG` command to set the intended password (See `CONFIG`).

YEDIS allows for multiple passwords (up to 2) to be accepted.
<li>If the given `password` matches with any of the server configured password(s), server returns the status string "OK" and begins processing commands from the authenticated client.</li>
<li>If the given `password` does not match with any of the server configured password(s), an error is raised</li>

## Return value

Returns a status string if the password is accepted. Returns an error if the password is rejected.

## Examples

```sh
$ CONFIG SET requirepass "yugapass"
```

```
"OK"
```

```sh
$ AUTH "yugapass"
```

```
"OK"
```

```sh
$ AUTH "bad"
```

```
"ERR: Bad Password."
```

```sh
$ CONFIG SET requirepass "yugapassA,yugapassB"
```

```
"OK"
```

```sh
$ AUTH "yugapassA"
```

```
"OK"
```

```sh
$ AUTH "yugapassB"
```

```
"OK"
```

```sh
$ AUTH "yugapassC"
```

```
"ERR: Bad Password."
```

## See lso

[`config`](../config/)
