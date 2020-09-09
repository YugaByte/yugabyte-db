---
title: do_demo.sql
linkTitle: do_demo.sql
headerTitle: do_demo.sql
description: do_demo.sql - Part of the code kit for the "Analyzing a normal distribution" section within the YSQL window functions documentation.
menu:
  latest:
    identifier: do-demo
    parent: analyzing-a-normal-distribution
    weight: 170
isTocNested: true
showAsideToc: true
---
Save this script as `do_demo.sql`.
```plpgsql
-- Uses table t4.
-- Once you've created it, you can run this script time and again using
-- for example, a different number of histogran buckets or a
-- different number of buckts for the analyses done by percent_rank(),
-- cum_dist(), and ntile().

--------------------------------------------------------------------------------
-- STEP ZERO
------------
-- Get a clean start.

\i do_clean_start.sql

--------------------------------------------------------------------------------
-- STEP ONE
------------
-- Create a function to report some useful overall measures of t4
-- and run it, spooling the output.

\i cr_show_t4.sql
\o reports/show_t4.txt
select t as "Some useful overall measures of t4."
from show_t4();
\o

\i cr_dp_views.sql
\i cr_pr_cd_equality_report.sql

\o reports/dp_pr_cd_equality_report.txt
select * from pr_cd_equality_report(0.50);
\t on
select * from pr_cd_equality_report(0.10);
select * from pr_cd_equality_report(0.05);
select * from pr_cd_equality_report(0.01);
\t off
\o

\i cr_int_views.sql

\o reports/int_pr_cd_equality_report.txt
select * from pr_cd_equality_report(0.50);
\t on
select * from pr_cd_equality_report(0.10);
select * from pr_cd_equality_report(0.05);
select * from pr_cd_equality_report(0.01);
\t off
\o

--------------------------------------------------------------------------------
-- STEP TWO
------------
-- Create a function to visualize the data as a histogram.
-- This relies on the function bucket().
-- Run it, spooling the output.

-- Choose one, then the other, of these two methods to
-- demonstrate that they produce identical results.
-- \i cr_bucket_using_width_bucket.sql
   \i cr_bucket_dedicated_code.sql

\i do_assert_bucket_ok.sql
\i cr_histogram.sql

\o reports/dp_histogram.txt
\t on
select * from histogram(50, 100);
\t off
\o

--------------------------------------------------------------------------------
-- STEP THREE
-------------
-- Compare the bucket allocation produced by ntile(), percent_rank(),
-- and cume_dist() acting on the double precision column dp_score.

\i cr_do_ntile.sql
\i cr_do_percent_rank.sql
\i cr_do_cume_dist.sql

\i cr_dp_views.sql
\i do_populate_results.sql
\o reports/dp_results.txt
\i do_report_results.sql
\o

\o reports/compare_dp_results.txt
\i do_compare_dp_results.sql
\o

-- STEP FOUR
-------------
-- Compare the bucket allocation produced by ntile(), percent_rank(),
-- and cume_dist() acting on the int column int_score.

\i cr_int_views.sql
\i do_populate_results.sql
\o reports/int_results.txt
\i do_report_results.sql
\o
```
