---
title: Configure Audit Logging in YCQL
headerTitle: Configure Audit Logging in YCQL
linkTitle: Configure Audit Logging in YCQL
description: Configure Audit Logging in YCQL.
headcontent: Configure Audit Logging in YCQL.
image: /images/section_icons/secure/authentication.png
menu:
  latest:
    name: Configure Audit Logging
    identifier: enable-audit-logging-2-ycql
    parent: audit-logging
    weight: 755
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/audit-logging/audit-logging-ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/latest/secure/audit-logging/audit-logging-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

Audit logging can be used to record information about YCQL statements or events (such as login events) and log the records on a per-node basis into the YB-Tserver logs. Audit logging can be enabled on YugabyteDB cluster by setting the` ycql_enable_audit_log `tserver flag to `true`. By Default, each TServer will record all login events and YCQL commands issued to the server.

Audit record is logged before an operation attempts to be executed, failures are audited as well. Hence, if an operation fails to execute, both operation execution and failure will be logged. However, an error that happens during parsing or analysis of YCQL statement will result only in a error audit record to be logged. 

YCQL Audit logging can be further customized by additional TServer flags described below.

## Enable Audit Logging

Audit logging for YCQL can be enabled by passing the `--ycql_enable_audit_log` flag to `yb-tserver`. The command to start the `yb-tserver` would look as follows:

```
$ yb-tserver <options> --ycql_enable_audit_log=true
```

## Configure Audit Logging

*   Statements or events are recorded if they match _all_ auditing filters described by the flags above. i.e. only the configured categories in the configured keyspaces by the configured users will be recorded.
*   For the `included` flags the default value (empty) means everything is included, while for the `excluded` flags the default value (empty) means nothing is excluded. So by default everything will be logged.
*   If both the inclusion and exclusion flags are set for the same dimension (e.g. users) then statements or events will be recorded only if both match: if they are in the set-difference between included entries and excluded entries. So that is allowed although it is redundant: the same semantics can be achieved by setting only the inclusion flag to the resulting set-difference. 
*   The `ycql_audit_log_level` determines the log file where the audit records will be written (i.e. `yb-tserver.INFO`, `yb-tserver.WARNING`, or `yb-tserver.ERROR`). \
Note that only `ERROR`-level logs are immediately flushed to disk, lower levels might be buffered.

## Audit Filters

### Objects being audited

TServer flags can be configured to determine which statements and events should be logged, audit logging can be configured along three different dimensions: categories (statement or event_)_ , users, and keyspaces.

Each of them can be configured either by inclusion (listing all statement categories, users or keyspaces to be audited) or by exclusion of CQL commands (listing all statement categories, user, or keyspaces to be excluded from auditing).

The available flags are described in the table below:


<table>
  <tr>
   <td><strong>Flag</strong>
   </td>
   <td><strong>Valid Values</strong>
   </td>
   <td><strong>Description</strong>
   </td>
  </tr>
  <tr>
   <td><code>ycql_enable_audit_log</code>
   </td>
   <td><code>false</code> (default) or <code>true</code>.
   </td>
   <td>Whether to enable YCQL audit, default <code>false</code>.
   </td>
  </tr>
  <tr>
   <td><code>ycql_audit_included_categories</code>
   </td>
   <td rowspan="2" >empty (default) or comma-separated list of statement categories.
   </td>
   <td>categories to be audited.
   </td>
  </tr>
  <tr>
   <td><code>ycql_audit_excluded_categories</code>
   </td>
   <td>categories to be excluded from auditing.
   </td>
  </tr>
  <tr>
   <td><code>ycql_audit_included_users</code>
   </td>
   <td rowspan="2" >empty (default) or comma-separated list of users.
   </td>
   <td>users to be audited.
   </td>
  </tr>
  <tr>
   <td><code>ycql_audit_excluded_users</code>
   </td>
   <td>users to be excluded from auditing.
   </td>
  </tr>
  <tr>
   <td><code>ycql_audit_included_keyspaces</code>
   </td>
   <td rowspan="2" >empty or comma-separated list of keyspaces.
<p>
Most system keyspaces are excluded by default.
   </td>
   <td>keyspaces to be audited.
   </td>
  </tr>
  <tr>
   <td><code>ycql_audit_excluded_keyspaces</code>
   </td>
   <td>keyspaces to be excluded from auditing.
   </td>
  </tr>
  <tr>
   <td><code>ycql_audit_log_level</code>
   </td>
   <td><code>INFO</code>, <code>WARNING</code>, or <code>ERROR</code> (default <code>ERROR</code>).
   </td>
   <td>Severity level at which an audit will be logged.
   </td>
  </tr>
</table>


All the flags above are `runtime` flags, so they can be set without requiring `yb-tserver` restart.

### Statements being audited

The valid statement categories are described in the table below.


<table>
  <tr>
   <td><strong>Audit Category</strong>
   </td>
   <td><strong>Covered YCQL statements or wire-protocol events</strong>
   </td>
  </tr>
  <tr>
   <td><code>QUERY</code>
   </td>
   <td><code>SELECT</code>
   </td>
  </tr>
  <tr>
   <td><code>DML</code>
   </td>
   <td><code>INSERT, UPDATE, DELETE, BEGIN TRANSACTION, </code>and batch statements.
   </td>
  </tr>
  <tr>
   <td><code>DDL</code>
   </td>
   <td><code>TRUNCATE, CREATE/ALTER/DROP KEYSPACE/TABLE/INDEX/TYPE </code>
   </td>
  </tr>
  <tr>
   <td><code>DCL</code>
   </td>
   <td><code>LIST USERS/ROLES/PERMISSIONS, GRANT, REVOKE, CREATE/ALTER/DROP ROLE</code>
   </td>
  </tr>
  <tr>
   <td><code>AUTH</code>
   </td>
   <td>Login error, login attempt, login success
   </td>
  </tr>
  <tr>
   <td><code>PREPARE</code>
   </td>
   <td>Prepared statement
   </td>
  </tr>
  <tr>
   <td><code>ERROR</code>
   </td>
   <td>Request failure
   </td>
  </tr>
  <tr>
   <td><code>OTHER</code>
   </td>
   <td><code>USE &lt;keyspace>, EXPLAIN</code>
   </td>
  </tr>
</table>

## Output Format

Log record for a `CREATE TABLE` statement executed by user `john`, on keyspace `prod`:


```
E0920 09:07:30.679694 10725 audit_logger.cc:552] AUDIT: user:john|
host:172.151.36.146:9042|source:10.9.80.22|port:56480|timestamp:1600592850679|
type:CREATE_TABLE|category:DDL|ks:prod|scope:test_table|operation:create table 
test_table(k int primary key, v int);
```

Each audit log record will have the following components:

<table>
  <tr>
   <td><strong>Field</strong>

   </td>
   <td><strong>Notes</strong>

   </td>
  </tr>
  <tr>
   <td><code>user</code></td>
   <td>User name (if available)</td>
  </tr>
  <tr>
   <td><code>host</code>
   </td>
   <td>IP of the node where the command is being executed
   </td>
  </tr>
  <tr>
   <td><code>source</code>
   </td>
   <td>IP address from where the request initiated
   </td>
  </tr>
  <tr>
   <td><code>port</code>
   </td>
   <td>Port number from where the request initiated
   </td>
  </tr>
  <tr>
   <td><code>timestamp</code>
   </td>
   <td>Unix timestamp (in milliseconds)
   </td>
  </tr>
  <tr>
   <td><code>type</code>
   </td>
   <td>Type of the request (`SELECT`, `INSERT`, etc.,)
   </td>
  </tr>
  <tr>
   <td><code>category</code>
   </td>
   <td>Category of the request (`DDL`, `DML`, etc.,)
   </td>
  </tr>
  <tr>
   <td><code>ks</code>
   </td>
   <td>Keyspace on which request is targeted to be executed (if applicable)
   </td>
  </tr>
  <tr>
   <td><code>scope</code>
   </td>
   <td>Target of the current operation, such as the table, user, type, or keyspace name for corresponding `CREATE`, `ALTER`, or `DROP` commands.
   </td>
  </tr>
  <tr>
   <td><code>operation</code>
   </td>
   <td>The YCQL command being executed.
   </td>
  </tr>
</table>

## Configuration Examples

This section shows some examples of how to configure audit logging.


##### Log all auth events


```
ycql_enable_audit_log=true
ycql_audit_included_categories=AUTH
```



##### Log all DDLs on keyspaces `ks1`


```
ycql_enable_audit_log=true
ycql_audit_included_categories=DDL 
ycql_audit_included_keyspace=ks1
```



##### Log all SELECTs and DMLs by `user1`


```
ycql_enable_audit_log=true
ycql_audit_included_categories=QUERY,DML
ycql_audit_included_users=user1 
```



##### Log DCLs by everyone except user `dbadmin`


```
ycql_enable_audit_log=true
ycql_audit_included_categories=DCL
ycql_audit_excluded_users=dbadmin
```


