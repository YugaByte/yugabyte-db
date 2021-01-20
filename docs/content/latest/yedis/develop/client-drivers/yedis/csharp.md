---
title: Build a YugabyteDB YEDIS application using C#
headerTitle: Build an application using C#
linkTitle: C#
description: Use C# to build a YugabyteDB application that interacts with YEDIS
aliases:
  - /latest/yedis/develop/client-drivers/csharp
menu:
  latest:
    identifier: client-drivers-yedis-csharp
    parent: develop-yedis
isTocNested: true
showAsideToc: true
---

## Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the Redis shell. If not, follow the steps in [Quick start](../../../../quick-start).
- installed Visual Studio.

## Write a HelloWorld C# application

In your Visual Studio create a new Project and choose Console Application as template. Follow the instructions to save the project.

### Install StackExchange.Redis C# driver

To install the driver in your Visual Studio project:

<ol>
  <li>Open your Project Solution View.</li>
  <li>Right-click on Packages and click Add Packages.</li>
  <li>Search for StackExchange.Redis and click Add Package.</li>
  ![Search Package](/images/develop/client-drivers/csharp/visual-studio-search-redis-package.png)
</ol>

### Copy the contents below to your `Program.cs` file.

```cs
using System;
using System.Collections.Generic;
using StackExchange.Redis;

namespace Yugabyte_CSharp_Demo
{
    class Program
    {
        static private void printHash(HashEntry[] hashes)
        {
            foreach (var hashEntry in hashes)
            {
                Console.WriteLine(string.Format("{0}: {1}", hashEntry.Name, hashEntry.Value));
            }
        }
        static void Main(string[] args)
        {
            try
            {
                ConfigurationOptions config = new ConfigurationOptions
                {
                    EndPoints =
                    {
                        { "127.0.0.1", 6379 },
                    },
                    CommandMap = CommandMap.Create(new HashSet<string>
                    {   // EXCLUDE commands that are not fully supported on YugabyteDB side.
                        "SUBSCRIBE", "CLUSTER", "TIME", "PING"
                    }, available: false)
                };

                ConnectionMultiplexer connection = ConnectionMultiplexer.Connect(config);
                IDatabase redisDB = connection.GetDatabase();
                var hashKey = "1";
                HashEntry[] setHash = {
                  new HashEntry("name", "John"),
                  new HashEntry("age", 35),
                  new HashEntry("language", "C#"),
                  new HashEntry("client", "Redis")
                };
                Console.WriteLine("Successfully executed HMSET:");
                printHash(setHash);
                redisDB.HashSet(hashKey, setHash);

                var getHash = redisDB.HashGetAll(hashKey);
                Console.WriteLine("Successfully executed HMGET:");
                printHash(getHash);
            }
            catch (RedisConnectionException e)
            {
                Console.WriteLine("Unable to make a connection to local YugabyteDB. " +
                                  "Error:", e.Message);
            }
        }
    }
}
```

### Run the application

Run the C# application from menu select `Run -> Start Without Debugging`

You should see the following as the output.

```
Successfully executed HMSET:
name: John
age: 35
language: C#
client: Redis
Successfully executed HMGET:
age: 35
client: Redis
language: C#
name: John
```
