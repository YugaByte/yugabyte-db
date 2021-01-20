---
title: Build a YugabyteDB application using C++ and YEDIS
headerTitle: Build an application using C++
linkTitle: C++
description: Use C++ to build a YugabyteDB application that interacts with YEDIS
aliases:
  - /latest/yedis/develop/client-drivers/cpp
menu:
  latest:
    identifier: client-drivers-yedis-cpp
    parent: develop-yedis
isTocNested: true
showAsideToc: true
---

## Prerequisites

The tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the Redis shell. If
  not, follow the steps in [Quick start](../../../../quick-start/).
- have C++ 11 or later.

## Install the Redis C++ driver

We use the [`cpp_redis`](https://redis.io/clients#c--) driver. To install the library do the following:

- Clone the `cpp_redis` repository
  
```sh
$ git clone https://github.com/Cylix/cpp_redis.git
```

- Get the networking module ([tacopie](https://github.com/Cylix/tacopie))

```sh
$ cd cpp_redis
$ git submodule init && git submodule update
```

- Create a build directory and move into it

```sh
$ mkdir build && cd build
```

- Generate the Makefile using CMake

```sh
$ cmake .. -DCMAKE_BUILD_TYPE=Release
```

- Build and install the library

```sh
$ make
$ make install
```

## Write the HelloWorld C++ application

Create a file `ybredis_hello_world.cpp` and copy the contents below:

```cpp
#include <cpp_redis/cpp_redis>

#include<iostream>
#include<vector>
#include<string>
#include<utility>
using namespace std;

int main() {
  cpp_redis::client client;

  client.connect("127.0.0.1", 6379, [](const std::string& host, std::size_t port, cpp_redis::client::connect_state status) {
    if (status == cpp_redis::client::connect_state::dropped) {
      std::cout << "client disconnected from " << host << ":" << port << std::endl;
    }
  });

  string userid = "1";
  vector<pair<string, string>> userProfile;
  userProfile.push_back(make_pair("name", "John"));
  userProfile.push_back(make_pair("age", "35"));
  userProfile.push_back(make_pair("language", "Redis"));

  // Insert the data
  client.hmset(userid, userProfile, [](cpp_redis::reply& reply) {
      cout<< "HMSET returned " << reply << ": id=1, name=John, age=35, language=Redis" << endl;
  });

  // Query the data
  client.hgetall(userid, [](cpp_redis::reply& reply) {
    std::vector<cpp_redis::reply> retVal;
    if (reply.is_array()) {
      retVal = reply.as_array();
    }
    cout << "Query result:" <<endl;
    for (int i = 0; i < retVal.size(); i=i+2) {
      cout << retVal[i] << "=" <<retVal[i+1] << endl;
    }
  });

  // synchronous commit, no timeout
  client.sync_commit();

  return 0;
}
```

## Run the application

To compile the file, run the following command.

```sh
$ g++ -ltacopie -lcpp_redis -std=c++11 -o ybredis_hello_world ybredis_hello_world.cpp
```

To run the application, run the following command.

```sh
$ ./ybredis_hello_world
```

You should see the following output.

```
HMSET returned OK: id=1, name=John, age=35, language=Redis
Query result:
age=35
language=Redis
name=John
```
