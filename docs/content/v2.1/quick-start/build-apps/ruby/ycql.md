---
title: Build a Ruby application that uses YCQL
headerTitle: Build a Ruby application
linkTitle: Ruby
description: Build a Ruby application that uses YCQL.
block_indexing: true
menu:
  v2.1:
    parent: build-apps
    name: Ruby
    identifier: ruby-3
    weight: 553
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/quick-start/build-apps/ruby/ysql-pg" class="nav-link">
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
    <a href="/latest/quick-start/build-apps/ruby/ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Installation

Install the Ruby YCQL driver using the following command. You can get further details for the driver [here](https://github.com/yugabyte/cassandra-ruby-driver).

```sh
$ gem install yugabyte-ycql-driver
```

## Working example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe and are able to interact with it using the YCQL shell. If not, please follow these steps in the [quick start guide](../../../../api/ycql/quick-start/).

### Writing the Ruby code

Create a file `yb-ycql-helloworld.rb` and add the following content to it.

```ruby
require 'ycql'

# Create the cluster connection, connects to localhost by default.
cluster = Cassandra.cluster
session = cluster.connect()

# Create the keyspace.
session.execute('CREATE KEYSPACE IF NOT EXISTS ybdemo;')
puts "Created keyspace ybdemo"

# Create the table.
session.execute(
  """
  CREATE TABLE IF NOT EXISTS ybdemo.employee (id int PRIMARY KEY,
                                              name varchar,
                                              age int,
                                              language varchar);
  """)
puts "Created table employee"

# Insert a row.
session.execute(
  """
  INSERT INTO ybdemo.employee (id, name, age, language)
  VALUES (1, 'John', 35, 'Ruby');
  """)
puts "Inserted (id, name, age, language) = (1, 'John', 35, 'Ruby')"

# Query the row.
rows = session.execute('SELECT name, age, language FROM ybdemo.employee WHERE id = 1;')
rows.each do |row|
  puts "Query returned: %s %s %s" % [ row['name'], row['age'], row['language'] ]
end

# Close the connection.
cluster.close()
```

### Running the application

To run the application, type the following:

```sh
$ ruby yb-cql-helloworld.rb
```

You should see the following output.

```
Created keyspace ybdemo
Created table employee
Inserted (id, name, age, language) = (1, 'John', 35, 'Ruby')
Query returned: John 35 Ruby
```
