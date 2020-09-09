---
title: Build a PHP application that uses YSQL
headerTitle: Build a PHP application
linkTitle: PHP
description: Build a PHP application that uses YSQL to perform basic database operations.
block_indexing: true
menu:
  v2.1:
    identifier: build-apps-php-1-ysql
    parent: build-apps
    weight: 555
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/build-apps/php/ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

</ul>

## Install the [`php-pgsql`](https://www.php.net/manual/en/book.pgsql.php) driver

See instructions to install [`php-pgsql`](https://www.php.net/manual/en/pgsql.installation.php) for your platform.

For example, for CentOS, use `sudo yum install php-pgsql` or Ubuntu use `sudo apt-get install php-pgsql`.

## Add sample PHP code

Create a file `yb-sql-sample.php` with the following content.

```php
<?php

try {
  /* Establish connection. */
  $dbh = new PDO('pgsql:host=127.0.0.1;port=5433;dbname=yugabyte;user=yugabyte;password=yugabyte',
                 'yugabyte', null, array(PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
                                         PDO::ATTR_EMULATE_PREPARES => true,
                                         PDO::ATTR_PERSISTENT => true));

  /* Create the table if it doesn't exist. */
  $stmt = 'CREATE TABLE IF NOT EXISTS employee (id int PRIMARY KEY,
                                                name   varchar,
                                                salary int,
                                                dept   varchar)';
  $dbh->exec($stmt);

  /* Prepare the insert statement. */
  $insert_stmt = $dbh->prepare('INSERT INTO employee(id, name, salary, dept) ' .
                               'VALUES (:id, :name, :salary, :dept)');

  /* Insert a row. */
  $insert_stmt->bindValue(':id', 10, PDO::PARAM_INT);
  $insert_stmt->bindValue(':name', 'Jane', PDO::PARAM_STR);
  $insert_stmt->bindValue(':salary', 150000, PDO::PARAM_INT);
  $insert_stmt->bindValue(':dept', 'Engineering', PDO::PARAM_STR);
  $insert_stmt->execute();

  /* Insert a row. */
  $insert_stmt->bindValue(':id', 11, PDO::PARAM_INT);
  $insert_stmt->bindValue(':name', 'Joe', PDO::PARAM_STR);
  $insert_stmt->bindValue(':salary', 140000, PDO::PARAM_INT);
  $insert_stmt->bindValue(':dept', 'Finance', PDO::PARAM_STR);
  $insert_stmt->execute();

  echo "Inserted new records successfully.\n";

  /* Prepare query statement to retrieve user info by id */
  $query = $dbh->prepare('SELECT name, salary, dept FROM employee WHERE id = :id');

  $query->bindValue(':id', 11, PDO::PARAM_INT);
  $query->execute();
  $user_info = $query->fetch(PDO::FETCH_ASSOC);

  echo "Retrieving info for user id 11...\n";
  print_r($user_info);

} catch (Exception $excp) {
  print "EXCEPTION: " . $excp->getMessage() . "\n";
  exit(1);
}
```

## Run the application

To run the application:

```sh
$ php yb-sql-sample.php
```

and you should see the following output:

```
Inserted new records successfully.
Retrieving info for user id 11...
Array
(
    [name] => Joe
    [salary] => 140000
    [dept] => Finance
)
```
