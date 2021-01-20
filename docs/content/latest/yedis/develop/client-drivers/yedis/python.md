---
title: Build a YugabyteDB application using Python and YEDIS
headerTitle: Build an application using Python
linkTitle: Python
description: Use Python to build a YugabyteDB application that interacts with YEDIS
aliases:
  - /latest/yedis/develop/client-drivers/python
menu:
  latest:
    identifier: client-drivers-yedis-python
    parent: develop-yedis
isTocNested: true
showAsideToc: true
---

## Install the Python driver

Install the Python driver using the following command.

```sh
$ sudo pip install yedis
```

## Working example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the Redis shell. If not, follow the steps in [Quick start](../../../../quick-start/).

### Write the HelloWorld Python application

Create a file `yb-redis-helloworld.py` and add the following content to it.

```python
import redis

# Create the cluster connection.
r = redis.Redis(host='localhost', port=6379)

# Insert the user profile.
userid = 1
user_profile = {"name": "John", "age": "35", "language": "Python"}
r.hmset(userid, user_profile)
print "Inserted userid=1, profile=%s" % user_profile

# Query the user profile.
print r.hgetall(userid)
```

### Run the application

To run the application, type the following:

```sh
$ python yb-redis-helloworld.py
```

You should see the following output.

```
Inserted userid=1, profile={'age': '35', 'name': 'John', 'language': 'Python'}
{'age': '35', 'name': 'John', 'language': 'Python'}
```
