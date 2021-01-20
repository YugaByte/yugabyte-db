---
title: Row-Level Security (RLS)
headerTitle: Row-Level Security (RLS)
linkTitle: Row-Level Security (RLS)
description: Row-Level Security (RLS) in YugabyteDB
menu:
  latest:
    name: Row-Level Security (RLS)
    identifier: ysql-row-level-security
    parent: authorization
    weight: 745
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/authorization/ysql-grant-permissions" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
</ul>

In addition to database access permissions available through ROLE and GRANT privilege system, YugabyteDB provides a more granular level security where tables can have **row security policies** that restrict rows users can access.

**Row-level Security (RLS)** restricts rows that can be returned by normal queries or inserted, updated, or deleted by DML commands. Row-level security policies can be created specific to a DML command or with ALL commands. They can also be used to create policies on a particular role or multiple roles.

By default, tables do not have any row level policies defined, so that if a user has access privileges to a table, all rows within the table are available to query and update.

This example uses the row-level security policies to restrict employees to view only rows that have their respective names.


## Step 1. Create example table

Open the YSQL shell (`ysqlsh`), specifying the `yugabyte` user and prompting for the password.


```
$ ./ysqlsh -U yugabyte -W
```


When prompted for the password, enter the yugabyte password. You should be able to login and see a response like below.


```
ysqlsh (11.2-YB-2.5.0.0-b0)
Type "help" for help.

yugabyte=#
```

Create a employee table and insert few sample rows


```
yugabyte=# create table employees ( empno int, ename text, address text, salary int,
           account_number text );
CREATE TABLE

yugabyte=# insert into employees values (1, 'joe', '56 grove st',  20000, 'AC-22001' );
INSERT 0 1
yugabyte=# insert into employees values (2, 'mike', '129 81 st',  80000, 'AC-48901' );
INSERT 0 1
yugabyte=# insert into employees values (3, 'julia', '1 finite loop',  40000, 'AC-77051');
INSERT 0 1

yugabyte=# select * from employees;
 empno | ename |    address    | salary | account_number
-------+-------+---------------+--------+----------------
     1 | Joe   | 56 grove st   |  20000 | AC-22001
     2 | Mike  | 129 81 st     |  80000 | AC-48901
     3 | Julia | 1 finite loop |  40000 | AC-77051
(3 rows)
```



## Step 2. Grant access to users

Set up the database by creating users based on the entries in rows and provide table access to them.


```
yugabyte=# create user joe;
CREATE ROLE
yugabyte=# grant select on employees to joe;
GRANT

yugabyte=# create user mike;
CREATE ROLE
yugabyte=# grant select on employees to mike;
GRANT

yugabyte=# create user julia;
CREATE ROLE
yugabyte=# grant select on employees to julia;
GRANT
```


At this point, users can see all the data


```
yugabyte=# \c yugabyte joe;
You are now connected to database "yugabyte" as user "joe".
yugabyte=> select * from employees;
 empno | ename |    address    | salary | account_number
-------+-------+---------------+--------+----------------
     1 | Joe   | 56 grove st   |  20000 | AC-22001
     3 | Julia | 1 finite loop |  40000 | AC-77051
     2 | Mike  | 129 81 st     |  80000 | AC-48901
(3 rows)

yugabyte=> \c yugabyte mike;
You are now connected to database "yugabyte" as user "mike".
yugabyte=> select * from employees;
 empno | ename |    address    | salary | account_number
-------+-------+---------------+--------+----------------
     1 | Joe   | 56 grove st   |  20000 | AC-22001
     3 | Julia | 1 finite loop |  40000 | AC-77051
     2 | Mike  | 129 81 st     |  80000 | AC-48901
(3 rows)

yugabyte=> \c yugabyte julia;
You are now connected to database "yugabyte" as user "julia".
yugabyte=> select * from employees;
 empno | ename |    address    | salary | account_number
-------+-------+---------------+--------+----------------
     1 | Joe   | 56 grove st   |  20000 | AC-22001
     3 | Julia | 1 finite loop |  40000 | AC-77051
     2 | Mike  | 129 81 st     |  80000 | AC-48901
(3 rows)
```



## Step 3. Setup RLS for a user

Now create a row-level security policy for user `joe`


```
yugabyte=> \c yugabyte yugabyte;
You are now connected to database "yugabyte" as user "yugabyte".

yugabyte=# CREATE POLICY emp_rls_policy ON employees FOR ALL TO PUBLIC USING (
           ename=current_user);
CREATE POLICY
```


Syntax of the `CREATE POLICY` command,

*   Use the `CREATE POLICY` command to create the policy. Need to be a superuser to execute this command.
*   `emp_rls_policy` is the user defined name for the policy.
*   `employees` is the name of the table.
*   `ALL` here represents all DDL commands. Alternatively one can specify select, insert, update, delete or other operations that need to be restricted.
*   `PUBLIC` here represents all roles. Alternatively one can provide specific role names to which the policy will apply.
*   `USING (ename=current_user)`is called the expression. It is a filter condition that returns a boolean value. This command compares the `ename` column of the `employees` tables to the **logged in** user, if they match then the user will be able to access the row for DDL operations. 


## Step 4. Enable RLS on table

Enable row-level security on the table


```
yugabyte=> \c yugabyte yugabyte;
You are now connected to database "yugabyte" as user "yugabyte".

yugabyte=# ALTER TABLE employees ENABLE ROW LEVEL SECURITY;
ALTER TABLE
```



## Step 5. Verify row-level security

Verify what each user can view from the employees table


```
yugabyte=# \c yugabyte yugabyte;
You are now connected to database "yugabyte" as user "yugabyte".
yugabyte=# select * from employees;
 empno | ename |    address    | salary | account_number
-------+-------+---------------+--------+----------------
     2 | mike  | 129 81 st     |  80000 | AC-48901
     1 | joe   | 56 grove st   |  20000 | AC-22001
     3 | julia | 1 finite loop |  40000 | AC-77051
(3 rows)

yugabyte=# \c yugabyte joe;
You are now connected to database "yugabyte" as user "joe".

yugabyte=> select current_user;
 current_user
--------------
 joe
(1 row)

yugabyte=> select * from employees;
 empno | ename |   address   | salary | account_number
-------+-------+-------------+--------+----------------
     1 | joe   | 56 grove st |  20000 | AC-22001
(1 row)

yugabyte=> \c yugabyte mike;
You are now connected to database "yugabyte" as user "mike".

yugabyte=> select current_user;
 current_user
--------------
 mike
(1 row)

yugabyte=> select * from employees;
 empno | ename |  address  | salary | account_number
-------+-------+-----------+--------+----------------
     2 | mike  | 129 81 st |  80000 | AC-48901
(1 row)

yugabyte=> \c yugabyte julia
You are now connected to database "yugabyte" as user "julia".

yugabyte=> select current_user;
 current_user
--------------
 julia
(1 row)

yugabyte=> select * from employees;
 empno | ename |    address    | salary | account_number
-------+-------+---------------+--------+----------------
     3 | julia | 1 finite loop |  40000 | AC-77051
(1 row)
```


As defined in the policy, the `current_user` can only access his or her own row.


## Step 6. Bypass row-level security

YugabyteDB has **BYPASSRLS** and **NOBYPASSRLS** permissions, which can be assigned to a role. By default, table owner and superuser have `BYPASSRLS` permissions assigned, so these users can skip the row-level security. The other roles in a database will have `NOBYPASSRLS` assigned to them by default.

Assign  `NOBYPASSRLS` to user `joe` so he can see all the rows in the employees table.


```
yugabyte=> \c yugabyte yugabyte;
You are now connected to database "yugabyte" as user "yugabyte".

yugabyte=# ALTER USER joe BYPASSRLS;
ALTER ROLE

yugabyte=# \c yugabyte joe;
You are now connected to database "yugabyte" as user "joe".

yugabyte=> select * from employees;
 empno | ename |    address    | salary | account_number
-------+-------+---------------+--------+----------------
     2 | mike  | 129 81 st     |  80000 | AC-48901
     1 | joe   | 56 grove st   |  20000 | AC-22001
     3 | julia | 1 finite loop |  40000 | AC-77051
(3 rows)
```



## Step 7. Remove row-level policy 

`DROP POLICY` command is used to drop a policy


```
yugabyte=> \c yugabyte yugabyte;
You are now connected to database "yugabyte" as user "yugabyte".

yugabyte=# DROP POLICY emp_rls_policy ON employees;
DROP POLICY
```


Logging in as user `joe` \ `julia` won’t return any data because the RLS policy was dropped and row-level security is still enabled on the table.


```
yugabyte=> \c yugabyte mike;
You are now connected to database "yugabyte" as user "mike".

yugabyte=> select current_user;
 current_user
--------------
 mike
(1 row)

yugabyte=> select * from employees;
 empno | ename | address | salary | account_number
-------+-------+---------+--------+----------------
(0 rows)
```


In order to completely disable row-level security on the table, `ALTER TABLE` to remove row-level security


```
yugabyte=> \c yugabyte yugabyte;
You are now connected to database "yugabyte" as user "yugabyte".

yugabyte=# ALTER TABLE employees DISABLE ROW LEVEL SECURITY;
ALTER TABLE

yugabyte=> \c yugabyte mike;
You are now connected to database "yugabyte" as user "mike".

yugabyte=> select current_user;
 current_user
--------------
 mike
(1 row)

yugabyte=> select * from employees;
 empno | ename |    address    | salary | account_number
-------+-------+---------------+--------+----------------
     2 | mike  | 129 81 st     |  80000 | AC-48901
     1 | joe   | 56 grove st   |  20000 | AC-22001
     3 | julia | 1 finite loop |  40000 | AC-77051
(3 rows)
```
