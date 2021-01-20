---
title: Download the COVIDcast data
linkTitle: Download the COVIDcast data
headerTitle: Finding and downloading the COVIDcast data
description: How to download data from Carnegie Mellon's COVIDcast project for linear regression analysis using YSQL
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: download-the-covidcast-data
    parent: covid-data-case-study
    weight: 10
isTocNested: true
showAsideToc: true
---

Simply follow these step-by-step instructions:

-  Create a directory on the computer where you run `ysqlsh` to hold the files for this case study. Call it, for example, _"covid-data-case-study"_.

- Go to the [COVIDcast site](https://covidcast.cmu.edu/) and select the “Export Data” tab. That will bring you to this screen:

![Download the COVIDcast Facebook Survey Data](/images/api/ysql/exprs/aggregate_functions/covid-data-case-study/covidcast-real-time-covid-19-indicators.png)

- **In Section #1**, _“Select Signal”_, select _“Facebook Survey Results”_ in the _“Data Sources”_ list and select _“People Wearing Masks”_ in the _“Signals”_ list.

- **In Section #2**, _“Specify Parameters”_, choose the range that interests you for _“Date Range”_ (This case study used _"2020-09-13 - 2020-11-01"_.) Select _“States”_ for the _“Geographic Level”_.

- **In Section #3**, _"Get Data”_ hit the _“CSV”_ button.

- **Then repeat**, leaving all choices unchanged except for the choice in the _“Signals”_ list. Select _“COVID-Like Symptoms”_ here.

- **Then repeat again**, again leaving all choices unchanged except for the choice in the _“Signals”_ list. Select _“COVID-Like Symptoms in Community”_ here.

   This will give you three files with names like these:

   ```
   covidcast-fb-survey-smoothed_wearing_mask-2020-09-13-to-2020-11-01.csv
   covidcast-fb-survey-smoothed_cli-2020-09-13-to-2020-11-01.csv
   covidcast-fb-survey-smoothed_hh_cmnty_cli-2020-09-13-to-2020-11-01.csv
   ```

   The naming convention is obvious. The names will reflect your choice of date range.
   
- Create a directory called _"csv-files_" on your  _"covid-data-case-study"_ directory and move the `.csv` files to this from your _"downloads"_ directory. Because you will not edit these files, you might like to make them all read-only to be sure that you don't make any accidental changes when you use a text editor or a spreadsheet app to inspect them.
