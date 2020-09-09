---
title: Build a Ruby application that uses YSQL
headerTitle: Build a Ruby application
linkTitle: Ruby
description: Build a Ruby application that uses Ruby PostgreSQL driver and YSQL.
block_indexing: true
menu:
  v2.1:
    parent: build-apps
    name: Ruby
    identifier: ruby-1
    weight: 553
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/quick-start/build-apps/ruby/ysql-pg" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - PG Gem
    </a>
  </li>
  <li >
    <a href="/latest/quick-start/build-apps/ruby/ysql-rails-activerecord" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - ActiveRecord
    </a>
  </li>
  <li>
    <a href="/latest/quick-start/build-apps/ruby/ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Install the pg driver gem

Install the Ruby PostgreSQL driver (`pg`) using the following command. You can get further details for the driver [here](https://bitbucket.org/ged/ruby-pg/wiki/Home).

```sh
$ gem install pg -- --with-pg-config=<yugabyte-install-dir>/postgres/bin/pg_config
```

## Working example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB and created a universe with YSQL enabled. If not, please follow these steps in the [Quick Start guide](../../../../quick-start/explore-ysql//).

### Writing the Ruby code

Create a file `yb-sql-helloworld.rb` and add the following content to it.

```python
#!/usr/bin/env ruby

require 'pg'

begin
  # Output a table of current connections to the DB
  conn = PG.connect(host: '127.0.0.1', port: '5433', dbname: 'yugabyte', user: 'yugabyte', password: 'yugabyte')

  # Create table
  conn.exec ("CREATE TABLE employee (id int PRIMARY KEY, \
                                     name varchar, age int, \
                                     language varchar)");

  puts "Created table employee\n";

  # Insert a row
  conn.exec ("INSERT INTO employee (id, name, age, language) \
                            VALUES (1, 'John', 35, 'Ruby')");
  puts "Inserted data (1, 'John', 35, 'Ruby')\n";

  # Query the row
  rs = conn.exec ("SELECT name, age, language FROM employee WHERE id = 1");
  rs.each do |row|
    puts "Query returned: %s %s %s" % [ row['name'], row['age'], row['language'] ]
  end

rescue PG::Error => e
  puts e.message
ensure
  rs.clear if rs
  conn.close if conn
end
```

### Running the application

To run the application, type the following:

```sh
$ ./yb-sql-helloworld.rb
```

You should see the following output.

```
Created table employee
Inserted data (1, 'John', 35, 'Ruby')
Query returned: John 35 Ruby
```
