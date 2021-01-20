---
title: Session-Level Audit Logging in YSQL
headerTitle: Session-Level Audit Logging in YSQL
linkTitle: Session-Level Audit Logging in YSQL
description: Session-Level Audit Logging in YSQL.
headcontent: Session-Level Audit Logging in YSQL.
image: /images/section_icons/secure/authentication.png
menu:
  latest:
    name: Session-Level Audit Logging
    identifier: session-audit-logging-1-ysql
    parent: audit-logging
    weight: 760
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/audit-logging/audit-logging-ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
</ul>

## Enable session-level audit

Session logging is enabled for per user session basis. Enable session logging for all DML and DDL statements and log all relations in DML statements.

```
set pgaudit.log = 'write, ddl';
set pgaudit.log_relation = on;
```


Enable session logging for all commands except MISC and raise audit log messages as NOTICE.

## Example

In this example session audit logging is used for logging DDL and SELECT statements. Note that the insert statement is not logged since the WRITE class is not enabled.

SQL statements are shown below.

### Step 1. Connect using `ysql`

Open the YSQL shell (ysqlsh), specifying the `yugabyte` user and prompting for the password.

```
$ ./ysqlsh -U yugabyte -W
```

When prompted for the password, enter the yugabyte password. You should be able to login and see a response like below.


```
ysqlsh (11.2-YB-2.5.0.0-b0)
Type "help" for help.

yugabyte=#
```


### Step 2. Enable `pgaudit` extension

Enable `pgaudit` extension on the YugabyteDB cluster.

```
yugabyte=> \c yugabyte yugabyte;
You are now connected to database "yugabyte" as user "yugabyte".

yugabyte=# CREATE EXTENSION IF NOT EXISTS pgaudit;
CREATE EXTENSION

```



### Step 3. Enable session audit logging

Enable session audit logging in YugabyteDB cluster.

```
set pgaudit.log = 'read, ddl';
```

### Step 4. Perform statements

```
create table account
(
    id int,
    name text,
    password text,
    description text
);

insert into account (id, name, password, description)
             values (1, 'user1', 'HASH1', 'blah, blah');

select *
    from account;
```


### Step 5. Verify output

You should see the following output in the logs:


```
2020-11-09 19:19:09.262 UTC [3710] LOG:  AUDIT: SESSION,1,1,DDL,CREATE 
TABLE,TABLE,public.account,"create table account
        (
            id int,
            name text,
            password text,
            description text
        );",<not logged>
2020-11-09 19:19:19.619 UTC [3710] LOG:  AUDIT: SESSION,2,1,READ,SELECT,,,"select *
            from account;",<not logged>
```
