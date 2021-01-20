---
title: Audit logging logging for the YCQL API
headerTitle: Audit logging for the YCQL API
linkTitle: Audit logging
description: Use audit logging in YugabyteDB for the YCQL API.
aliases:
  - /stable/secure/audit-logging
menu:
  stable:
    identifier: audit-logging-2-ycql
    parent: secure
    weight: 750
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./ycql.md" >}}" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

This document describes how to enable and configure audit logging in YugabyteDB for the YCQL API.

## Synopsis

Audit logging can be used to record information about YCQL statements or events (such as login events) and write the records to a per-node log (the tserver log).

You can enable audit logging by setting the YB-TServer `ycql_enable_audit_log` configuration flag to `true` for the universe. Additional YB-TServer configuration flags can also be used, as described below.

By default, each node will record all login events and YCQL commands issued to it. Each audit record is logged before an operation attempts to be executed and failures are audited as well. If an operation fails to execute, both the operation execution and its failure will be logged. If an error occurs during parsing or analysis, it will only result in an error audit record.

## Configuration

You can configure which statements and events are logged along three orthogonal dimensions: categories (statement or event), users, and keyspaces. Each of these can be configured either by inclusion (listing all statement categories, users, or keyspaces to be audited) or by exclusion (listing all statement categories, users, or keyspaces to be excluded from auditing).

The available configuration flags are described in the following table:

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

All the flags above are `runtime` flags, so they can be set without requiring node (tserver) restart.

The valid statement categories are described in the table below.

<table>
  <tr>
   <td><strong>Audit Category</strong>
   </td>
   <td><strong>Covered <code>YCQL statements</code> or wire-protocol events</strong>
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

## Output

Each audit log record will have the following components:

<table>
  <tr>
   <td><strong>Field</strong>
   </td>
   <td><strong>Notes</strong>
   </td>
  </tr>
  <tr>
   <td><code>user</code>
   </td>
   <td>User name (if available)
   </td>
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
   <td>Type of the request (<code>SELECT</code>, <code>INSERT</code>, etc.,)
   </td>
  </tr>
  <tr>
   <td><code>category</code>
   </td>
   <td>Category of the request (<code>DDL</code>, <code>DML</code>, etc.,)
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
   <td>Target of the current operation, such as the table, user, type, or keyspace name for corresponding <code>CREATE</code>, <code>ALTER</code>, or <code>DROP</code> commands.
   </td>
  </tr>
  <tr>
   <td><code>operation</code>
   </td>
   <td>The YCQL command being executed.
   </td>
  </tr>
</table>

### Example

Here is a log record for a `CREATE TABLE` statement executed by user `john`, on keyspace `prod`:

```
E0920 09:07:30.679694 10725 audit_logger.cc:552] AUDIT: user:john|host:172.151.36.146:9042|source:10.9.80.22|port:56480|timestamp:1600592850679|type:CREATE_TABLE|category:DDL|ks:prod|scope:test_table|operation:create table test_table(k int primary key, v int);
```

## Semantics

* Statements or events are recorded if they match _all_ auditing filters described by the flags above. Only the configured categories in the configured keyspaces by the configured users will be recorded.
* For `included` flags, the default value (empty) means that everything is included.
* For `excluded` flags, the default value (empty) means nothing is excluded. Thus, by default, everything will be logged.
* If both the inclusion and exclusion flags are set for the same dimension (for example, users), then the statements or events will only be recorded if both match: if they are in the set-difference between included entries and excluded entries. That is allowed even though it is redundant: the same semantics can be achieved by setting only the inclusion flag to the resulting set-difference.
* The `--ycql_audit_log_level` configuration flag determines the log file where the audit records will be written (that is, `yb-tserver.INFO`, `yb-tserver.WARNING`, or `yb-tserver.ERROR`). Note that only `ERROR`-level logs are immediately flushed to disk, while the lower levels might be buffered.

### Examples

#### All auth events

```
ycql_enable_audit_log=true
ycql_audit_included_categories=AUTH
```

#### All DDLs on keyspaces `ks1`

```
ycql_enable_audit_log=true
ycql_audit_included_categories=DDL 
ycql_audit_included_keyspace=ks1
```

#### All SELECTs and DMLs by `user1`

```
ycql_enable_audit_log=true
ycql_audit_included_categories=QUERY,DML
ycql_audit_included_users=user1 
```

#### DCLs by everyone except user `dbadmin`

```
ycql_enable_audit_log=true
ycql_audit_included_categories=DCL
ycql_audit_excluded_users=dbadmin
```
