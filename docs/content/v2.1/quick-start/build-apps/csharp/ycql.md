---
title: Build a C# application that uses YCQL
headerTitle: Build a C# application
linkTitle: C#
description: Build a C# application that uses YCQL.
block_indexing: true
menu:
  v2.1:
    identifier: build-apps-csharp-2-ycql
    parent: build-apps
    weight: 554
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/build-apps/csharp/ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li>
    <a href="/latest/quick-start/build-apps/csharp/ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

## Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe and are able to interact with it using the YCQL shell. If not, please follow these steps in the [Quick start YCQL](../../../../quick-start/test-cassandra/).
- installed Visual Studio

## Writing a HelloWorld C# app

In your Visual Studio create a new Project and choose Console Application as template. Follow the instructions to save the project.

### Install YugaByteCassandraCSharpDriver C# driver

YugabyteDB has forked the Cassandra driver to add more features like JSONB and change the routing policy.
[Install the C# driver](https://www.nuget.org/packages/YugaByteCassandraCSharpDriver/) in your Visual Studio project by
following instructions on the page.

### Copy the contents below to your `Program.cs` file.

```cs
using System;
using System.Linq;
using Cassandra;

namespace Yugabyte_CSharp_Demo
{
    class Program
    {
        static void Main(string[] args)
        {
            try
            {
                var cluster = Cluster.Builder()
                                     .AddContactPoints("127.0.0.1")
                                     .WithPort(9042)
                                     .Build();
                var session = cluster.Connect();
                session.Execute("CREATE KEYSPACE IF NOT EXISTS ybdemo");
                Console.WriteLine("Created keyspace ybdemo");

                var createStmt = "CREATE TABLE IF NOT EXISTS ybdemo.employee(" +
                    "id int PRIMARY KEY, name varchar, age int, language varchar)";
                session.Execute(createStmt);
                Console.WriteLine("Created keyspace employee");

                var insertStmt = "INSERT INTO ybdemo.employee(id, name, age, language) " +
                    "VALUES (1, 'John', 35, 'C#')";
                session.Execute(insertStmt);
                Console.WriteLine("Inserted data: {0}", insertStmt);

                var preparedStmt = session.Prepare("SELECT name, age, language " +
                                                   "FROM ybdemo.employee WHERE id = ?");
                var selectStmt = preparedStmt.Bind(1);
                var result = session.Execute(selectStmt);
                var rows = result.GetRows().ToList();
                Console.WriteLine("Select query returned {0} rows", rows.Count());
                Console.WriteLine("Name\tAge\tLanguage");
                foreach (Row row in rows)
                    Console.WriteLine("{0}\t{1}\t{2}", row["name"], row["age"], row["language"]);

                session.Dispose();
                cluster.Dispose();

            }
            catch (Cassandra.NoHostAvailableException)
            {
                Console.WriteLine("Make sure YugabyteDB is running locally!.");
            }
            catch (Cassandra.InvalidQueryException ie)
            {
                Console.WriteLine("Invalid Query: " + ie.Message);
            }
        }
    }
}
```

### Running the C# app
Run the C# app from menu select `Run -> Start Without Debugging`

You should see the following as the output.

```
Created keyspace ybdemo
Created keyspace employee
Inserted data: INSERT INTO ybdemo.employee(id, name, age, language) VALUES (1, 'John', 35, 'C#')
Select query returned 1 rows
Name	Age	Language
John	35	C#
```
