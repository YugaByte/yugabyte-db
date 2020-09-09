---
title: Build a Ruby application that uses YCQL
headerTitle: Build a Ruby application
linkTitle: Ruby
description: Build a sample Ruby application with the Yugabyte Ruby Driver for YCQL.
menu:
  latest:
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
    <a href="{{< relref "./ysql-pg.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - PG Gem
    </a>
  </li>
  <li >
    <a href="{{< relref "./ysql-rails-activerecord.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL - ActiveRecord
    </a>
  </li>
  <li>
    <a href="{{< relref "./ycql.md" >}}" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

## Install the Yugabyte Ruby Driver for YCQL

To install the [Yugabyte Ruby Driver for YCQL](https://github.com/yugabyte/cassandra-ruby-driver), run the following `gem install` command:

```sh
$ gem install yugabyte-ycql-driver
```

## Create a sample Ruby application

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the YCQL shell. If not, follow the steps in [Quick start YCQL](../../../../api/ycql/quick-start/).

### Write the sample Ruby application

Create a file `yb-ycql-helloworld.rb` and copy the following content to it.

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

### Run the application

To use the application, run the following command:

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
