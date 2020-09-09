---
title: Build a C# application that uses YSQL
headerTitle: Build a C# application
linkTitle: C#
description: Use C# to build a YugabyteDB application that uses YSQL
block_indexing: true
menu:
  v2.1:
    identifier: build-apps-csharp-1-ysql
    parent: build-apps
    weight: 554
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/build-apps/csharp/ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li>
    <a href="/latest/quick-start/build-apps/csharp/ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

## Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the YSQL shell (`ysqlsh`). If not, follow the steps in [Quick start](../../../../quick-start).
- installed Visual Studio

## Writing a HelloWorld C# application

In your Visual Studio, create a new Project and choose **Console Application as template**. Follow the instructions to save the project.

### Install YSQL C# driver

To install the driver in your Visual Studio project:

<ol>
  <li>Open your Project Solution View.</li>
  <li>Right-click on **Packages** and click **Add Packages**.</li>
  ![Add Package](/images/develop/client-drivers/csharp/visual-studio-add-package.png) 
  <li>Search for Npgsql and click **Add Package**.</li>
  ![Search Package](/images/develop/client-drivers/csharp/visual-studio-search-ngpsql-package.png)
</ol>

### Copy the contents below to your `Program.cs` file

```cs
using System;
using Npgsql;

namespace Yugabyte_CSharp_Demo
{
    class Program
    {
        static void Main(string[] args)
        {
            NpgsqlConnection conn = new NpgsqlConnection("host=localhost;port=5433;database=yb_demo;user id=yugabyte;password=");

            try
            {
                conn.Open();

                NpgsqlCommand empCreateCmd = new NpgsqlCommand("CREATE TABLE employee (id int PRIMARY KEY, name varchar, age int, language varchar);", conn);
                empCreateCmd.ExecuteNonQuery();
                Console.WriteLine("Created table Employee");

                NpgsqlCommand empInsertCmd = new NpgsqlCommand("INSERT INTO employee (id, name, age, language) VALUES (1, 'John', 35, 'CSharp');", conn);
                int numRows = empInsertCmd.ExecuteNonQuery();
                Console.WriteLine("Inserted data (1, 'John', 35, 'CSharp')");

                NpgsqlCommand empPrepCmd = new NpgsqlCommand("SELECT name, age, language FROM employee WHERE id = @EmployeeId", conn);
                empPrepCmd.Parameters.Add("@EmployeeId", NpgsqlTypes.NpgsqlDbType.Integer);

                empPrepCmd.Parameters["@EmployeeId"].Value = 1;
                NpgsqlDataReader reader = empPrepCmd.ExecuteReader();

                Console.WriteLine("Query returned:\nName\tAge\tLanguage"); 
                while (reader.Read())
                {
                    Console.WriteLine("{0}\t{1}\t{2}", reader.GetString(0), reader.GetInt32(1), reader.GetString(2));
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine("Failure: " + ex.Message);
            }
            finally
            {
                if (conn.State != System.Data.ConnectionState.Closed)
                {
                    conn.Close();
                }
            }
        }
    }
}
```

### Run the C# application

Run the C# app from menu select `Run -> Start Without Debugging`

You should see the following as the output.

```
Created table Employee
Inserted data (1, 'John', 35, 'CSharp')
Query returned:
Name	Age	Language
John	35	CSharp
```
