---
title: Build a C++ application that uses YSQL
headerTitle: Build a C++ application
linkTitle: C++
description: Build a C++ application that uses the YSQL API.
block_indexing: true
menu:
  v2.1:
    identifier: build-apps-cpp-1-ysql
    parent: build-apps
    weight: 556
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/build-apps/cpp/ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li>
    <a href="/latest/quick-start/build-apps/cpp/ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

## Prerequisites

The tutorial assumes that you have:

- installed YugabyteDB, and created a universe with YSQL enabled. If not, please follow the steps in [Quick start](../../../explore-ysql).
- have a 32-bit (x86) or 64-bit (x64) architecture machine.
- have `gcc` 4.1.2 or later, clang 3.4 or later installed.

## Install the C++ driver (libpqxx)

Download the source from the [libpqxx](https://github.com/jtv/libpqxx) and build the binaries as follows. If needed, detailed steps are provided in the [README](https://github.com/jtv/libpqxx/blob/master/README.md) file.

### Get the source

```sh
$ git clone https://github.com/jtv/libpqxx.git
```

### Dependencies

Note that this package depends on PostgreSQL binaries. Make sure that the PostgreSQL `bin` directory is on the command path.

```sh
export PATH=$PATH:<yugabyte-install-dir>/postgres/bin
```

### Build and install

```sh
$ cd libpqxx
$ ./configure
$ make
$ make install
```

## Working example

### Sample C++ code

Create a file `ybsql_hello_world.cpp` and copy the contents below:

```cpp
#include <iostream>
#include <pqxx/pqxx>

int main(int, char *argv[])
{
  pqxx::connection c("host=127.0.0.1 port=5433 dbname=yugabyte user=yugabyte password=yugabyte");
  pqxx::work txn(c);
  pqxx::result r;

  /* Create table */
  try
  {
    r = txn.exec("CREATE TABLE employee (id int PRIMARY KEY, \
                  name varchar, age int, \
                  language varchar)");
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  std::cout << "Created table employee\n";

  /* Insert a row */
  try
  {
    r = txn.exec("INSERT INTO employee (id, name, age, language) \
                  VALUES (1, 'John', 35, 'C++')");
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  std::cout << "Inserted data (1, 'John', 35, 'C++')\n";

  /* Query the row */
  try
  {
    r = txn.exec("SELECT name, age, language FROM employee WHERE id = 1");

    for (auto row: r)
      std::cout << "Query returned: "
          << row["name"].c_str() << ", "
          << row["age"].as<int>() << ", "
          << row["language"].c_str() << std::endl;
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  txn.commit();
  return 0;
}
```

### Run the application

You can compile the file using `gcc` or `clang`. Note that C++ 11 is the minimum supported C++ version. Make sure your compiler supports this, and if necessary, that you have support for C++11 configured. For gcc, you can use:

```sh
$ g++ -std=c++11 ybsql_hello_world.cpp -lpqxx -lpq -I<yugabyte-install-dir>/postgres/include -o ybsql_hello_world
```

Run with:

```sh
$ ./ybsql_hello_world
```

You should see the following output:

```
Created table employee
Inserted data (1, 'John', 35, 'C++')
Query returned: John, 35, C++
```
