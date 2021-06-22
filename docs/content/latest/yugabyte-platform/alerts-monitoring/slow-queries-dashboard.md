---
title: Slow Queries dashboard
headerTitle: Slow Queries dashboard
linkTitle: Slow Queries dashboard
description: Slow Queries dashboard
menu:
  latest:
    parent: alerts-monitoring
    identifier: slow-queries-dashboard
    weight: 10
isTocNested: true
showAsideToc: true
---

Use the Slow Queries dashboard to analyze statistics of past queries on your YugabyteDB universes. You can use this data to:

- Visually identify slower running database operations
- Evaluate query execution times over time
- Discover potential queries for memory optimization

All user roles — `Super Admin`, `Admin`, and `Read-only` — are granted access to use the Slow Queries dashboard.

{{< note title="Note" >}}

Note that slow queries are not available for YCQL.

{{< /note >}}

## Columns description

| Column                     | Description                                                  |
| -------------------------- | ------------------------------------------------------------ |
| Query                      | The query command. Example: `select * from my_keyspace.my_table` |
| Database                   | The YSQL database used by the query         |
| User                       | The name of role used to access YSQL database         |
| Count                      | Total number of times this type of query has executed         |
| Total time                 | Total duration (in milliseconds) of all iterations of this query has taken         |
| Rows                       | The total number of database table rows returned across all iterations of this query  |
| Avg Exec Time              | Average or mean execution time (in milliseconds) for this query    |
| Min Exec Time              | Minimum execution time (in milliseconds) for this query    |
| Max Exec Time              | Maximum execution time (in milliseconds) for this query    |
| Std Dev Time               | Standard deviation of execution times for this query    |
| Temp Tables RAM            | Memory used by temporary tables generated from query    |

## Use the **Slow Queries** dashboard

1. Go to the **Universe Details** page and from the **Queries** tab, select **Slow Queries**.

![Initial layout](/images/yp/alerts-monitoring/slow-queries/initial-table-view.png)

2. Clicking the 'x' will close the alert that slow queries are not available on YCQL. The Column Display allows for dynamically displaying specific fields.

![Changing column selection](/images/yp/alerts-monitoring/slow-queries/selecting-columns.png)

3. Clicking the minimize icon will hide away the Column Display, allowing for more space to examine the query rows.

![Minimized columns panel](/images/yp/alerts-monitoring/slow-queries/minimized-columns-panel.png)

4. Click the search bar and the column filter drop-down list appears. The column filter drop-down list lets you use a query language for filtering data based on certain fields.

![Search dropdown options](/images/yp/alerts-monitoring/slow-queries/search-dropdown-options.png)

Use filtering for comparisons on numbers columns (`Avg Time`) using `>`, `>=`, `<`, and `<=` to search for values that are greater than, greater than or equal to, less than, and less than or equal to another value (`Avg Time: < 30`). You can also use the range syntax `n..m` to search for values within a range, where the first number `n` is the lowest value and the second number `m` is the highest value. The range syntax supports tokens like the following: `n..*` which is equivalent to `>= n`. Or `*..n` which is the same as `<= n`.

5. Click on a row to open a sidebar with a full view of the query statement, along with all the column data.

![View query statement](/images/yp/alerts-monitoring/slow-queries/query-info-panel.png)

You can also find additional prefiltered navigation links from different pages to the **Slow Queries** page.

* Example: From the **Overview** tab to the **Queries** tab, when the user clicks the link to "Top SQL Statements".

![Overview page showing slow queries](/images/yp/alerts-monitoring/slow-queries/overview-showing-link.png)

* Example: The **Nodes** page each node's Actions contains a link to the **Slow Queries** page.

![Nodes page showing link](/images/yp/alerts-monitoring/live-queries/nodes-page-show-link.png)
