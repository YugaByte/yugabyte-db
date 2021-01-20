---
title: Enable Users in YEDIS
headerTitle: Enable Users in YEDIS
linkTitle: Enable Users in YEDIS
description: Enable Users in YEDIS.
headcontent: Enable Users in YEDIS.
image: /images/section_icons/secure/authentication.png
menu:
  latest:
    name: Enable User Authentication
    identifier: enable-authentication-3-yedis
    parent: enable-authentication
    weight: 715
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/enable-authentication/ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/latest/secure/enable-authentication/ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
  <li>
    <a href="/latest/secure/enable-authentication/yedis" class="nav-link active">
      <i class="icon-redis" aria-hidden="true"></i>
      YEDIS
    </a>
  </li>
</ul>

YEDIS authentication is based on passwords. Each client connecting using the YEDIS API should provide a valid password in order to execute any command successfully.

**NOTE:** YEDIS implements a password-only authentication scheme. From the [Redis security docs page ("Authentication feature" section)](https://redis.io/topics/security), the open source version of Redis does not try to implement authentication, providing a small layer of authentication that can optionally be enabled in the `redis.conf` file.

## 1. Enable YEDIS authentication

You can enable password-based authentication in YEDIS API using the [CONFIG](../../../api/yedis/config/) command.

To do so, connect to the cluster using `redis-cli` and run the following command:

```sql
127.0.0.1:6379> CONFIG SET requirepass "password"
```

```
"OK"
```

## 2. Connect with redis-cli

Next exit `redis-cli`, connect to the cluster again using `redis-cli` and run the `PING` command (or any other command).

```sql
127.0.0.1:6379> PING
```

```
(error) NOAUTH PING: Authentication required.
```

You would need to authenticate the client (`redis-cli` in this case) by running the [AUTH](../../../api/yedis/auth/) command:

```sql
127.0.0.1:6379> AUTH password
```

```
"OK"
```

Subsequently, running any command would succeed:

```sql
127.0.0.1:6379> PING
```

```
PONG
```

## 3. Changing authentication credentials

YEDIS allows for multiple passwords (up to 2) to be accepted. This enables performing a graceful change of password without experiencing any application outage. Note that this requires knowing the old password.

Let us assume that the old password is `old-password` and the new password you intend to change it to is `new-password`. The preferred sequence is:

- Add a new password

```sql
127.0.0.1:6379> CONFIG SET requirepass "old-password,new-password"
```

This enables connecting to the database using both passwords.

- Change password used by the application tier

This would involve changing the config or pushing an updated binary to the application tier so that it now connects using `new-password`.

- Drop old password

```sql
127.0.0.1:6379> CONFIG SET requirepass "new-password"
```
