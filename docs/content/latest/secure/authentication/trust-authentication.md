---
title: Trust Authentication
headerTitle: Trust Authentication
linkTitle: Trust Authentication
description: Allow client to connect using a database user name.
headcontent: Configure trust authentication YSQL clients
image: /images/section_icons/secure/authentication.png
menu:
  latest:
    identifier: trust-authentication
    parent: authentication
    weight: 734
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/authentication/ysql-authentication" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
</ul>

{{< warning title="Warning" >}}
`trust` authentication is only suitable if you trust every user on every machine allowed to connect to the server by the ysql_hba.conf lines that specify trust. It is seldom reasonable to use trust for any TCP/IP connections other than those from localhost (127.0.0.1).
{{< /warning >}}

When `trust` authentication is specified, YugabyteDB assumes that any user trying to connect with the YB-TServer can access the database with the database user name they specify. This method is appropriate and very convenient for local connections to the YugabyteDB cluster.

By default YugabyteDB cluster uses `trust `authentication. One can explicitly specify `trust` authentication by setting with the following <code>[--ysql_hba_conf](https://docs.yugabyte.com/latest/reference/configuration/yb-tserver/#ysql-hba-conf)</code> flag.


```
--ysql_hba_conf="host all all 0.0.0.0/0 trust, host all all ::0/0 trust"
```

This enables `trust` authentication for all users. 
