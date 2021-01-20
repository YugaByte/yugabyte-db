---
title: Inspect the COVIDcast .csv files
linkTitle: Inspect the COVIDcast data
headerTitle: Inspect the COVIDcast .csv files
menu:
  latest:
    identifier: inspect-the-csv-files
    parent: ingest-the-covidcast-data
    weight: 10
isTocNested: true
showAsideToc: true
---

## What the columns in the .csv files mean

Open each of the `.csv` files that you downloaded in a plain text editor—or, if you prefer, a spreadsheet app. You'll see that there are twelve columns and that the first row is the title row. The first column has no title but you'll see that the values are all integers. This column will be named _"code"_ in each of the three staging tables into which the values from the `.csv` files are loaded.

Here, using that naming choice for the first column, is the list of column names. The list shows a typical value for each column.

```
code:         0
geo_value:    ak
signal:       smoothed_wearing_mask
time_value:   2020-09-26
direction:    <null>
issue:        2020-10-05
lag:          9
value:        79.4036061
stderr:       1.5071256
sample_size:  720.0
geo_type:     state
data_source:  fb-survey
```

The meanings of the columns are given, on the COVIDcast site, within the documentation for the [R package](https://cmu-delphi.github.io/covidcast/covidcastR/reference/covidcast_signal.html#value). The section [ILI and CLI Indicators](https://cmu-delphi.github.io/delphi-epidata/api/covidcast-signals/fb-survey.html#ili-and-cli-indicators) explains how to interpret the values in the _"value"_ column. Notice this on the page from which you downloaded the data:

> Each geographic region is identified with a unique identifier, such as FIPS code. See the [geographic coding documentation](https://cmu-delphi.github.io/delphi-epidata/api/covidcast_geography.html) for details.

Some of the explanations given on these referenced pages are more than you need. Here, for your convenience, is a précis—at the level of detail that this case study needs.

### "code" and "geo_value"

You can see immediately that _"geo_value"_ contains the 50 familiar two-letter abbreviations for the US States together with DC for the District of Columbia making 51 distinct values in all. And you can see, too, that _"code"_ contains integer values in the range _0..50_. The procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) will check that these two rules hold and that the values for _"code"_ and _"geo_value"_ bear a fixed one-to-one correspondence. In the general case, these two columns have a meaning that reflects the choice that you made for _"Geographic Level"_ when you downloaded the `.csv` files.

[Here is the code](../check-data-conforms-to-the-rules/#the-geo-value-column) from the procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) that checks that there are _51_ distinct values of _"geo_value"_ in each staging table as a whole and that they correspond to a manually provided list of the actual states (including DC).

[This code](../check-data-conforms-to-the-rules/#check-that-each-survey-date-has-the-same-number-of-states) checks that there are _51_ distinct values of _"geo_value"_ for each distinct _"time_value_.

[This code](../check-data-conforms-to-the-rules/#check-the-one-to-one-correspondence-between-code-and-geo-code)  checks the one-to-one correspondence between _"code"_ and _"geo_value"_.

### signal

Every row in this column, for a particular `.csv` file, has the same value, thus:

```
covidcast-fb-survey-smoothed_wearing_mask-2020-09-13-to-2020-11-01.csv — smoothed_wearing_mask
covidcast-fb-survey-smoothed_cli-2020-09-13-to-2020-11-01.csv          — smoothed_cli
covidcast-fb-survey-smoothed_hh_cmnty_cli-2020-09-13-to-2020-11-01.csv — smoothed_hh_cmnty_cli
```

[Here is the code](../check-data-conforms-to-the-rules/#the-signal-column) from the procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) that checks that this rule holds.

### time_value

You'll see that this column, in each `.csv` file, has the same set of distinct values: the _50_ dates in the range _2020-09-13 - 2020-11-01_. In other words, the value specifies the date of the Facebook survey.

[Here is the code](../check-data-conforms-to-the-rules/#the-time-value-column) from the procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) that checks that this rule holds.

[This code](../check-data-conforms-to-the-rules/#check-that-each-state-has-the-same-number-of-survey-dates) checks that there are _50_ distinct values of _"time_value"_ (in a dense series) for each distinct _"geo_value"_.

### direction

This column, in each `.csv` file, is empty—and this will be reflected by the fact that the columns in the three tables into which the three files are ingested are `NULL` for every row.

[Here is the code](../check-data-conforms-to-the-rules/#the-direction-column) from the procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) that checks that this rule holds.

### "issue" and "lag"

Briefly, these values specify the publication date for the data and the number of days that the publication date lagged the date of the observation. This case study doesn't use these values.

### value

This is given as a percentage in each of the three `.csv` files. The procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) will check that the values lie in the range _0..100_. The page from which you downloaded the data explains, when each of the signals that you downloaded was highlighted, the significance of that signal's value.

- _People Wearing Masks:_ Percentage of people who wear a mask most or all of the time while in public, based on surveys of Facebook users.
- _COVID-Like Symptoms:_ Percentage of people with COVID-like symptoms, based on surveys of Facebook users.
- _COVID-Like Symptoms in Community:_ Percentage of people who know someone in their local community with COVID-like symptoms, based on surveys of Facebook users.

The percentages reflect the ratio of the number of respondents who answered "yes" to the number of respondents to the particular survey question.

Notice the use of the term of art "smoothed" in the names of the `.csv` files and the values of "_signal"_. The [ILI and CLI Indicators](https://cmu-delphi.github.io/delphi-epidata/api/covidcast-signals/fb-survey.html#ili-and-cli-indicators) page explains it thus:

> The smoothed versions of the signals... are calculated using seven day pooling. For example, the estimate reported for June 7 in a specific geographical area... is formed by collecting all surveys completed between June 1 and 7 (inclusive)...

In other words, all values are seven-day trailing moving averages.

[Here is the code](../check-data-conforms-to-the-rules/#the-value-column) from the procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) that checks that these values are in the legal range for percentages, _0..100_.

### stderr

This gives a confidence measure for the moving averages produced by smoothing. It isn't used by this case study, but it is carried over to the  _"covidcast_fb_survey_results"_ table that represents the data that (apart from this one and _"sample_size"_ for each signal) the case study uses. You might find these values interesting for your own _ad hoc_ queries.

### sample_size

This specifies the number of respondents, on a particular day. It isn't used by this case study, but it is carried over to the  _"covidcast_fb_survey_results"_ table that represents the data that (apart from this one and _"stderr"_ for each signal) the case study uses. You might find these values interesting for your own _ad hoc_ queries.

### "data_source" and "geo_type"

The values in each of these two columns are the same for every row in the three `.csv` files. They reflect your choice to download _“Facebook Survey Results”_ at the _"States"_ geographic level. The values are, respectively, _'fb-survey'_ and _'state'_.

[Here is the code](../check-data-conforms-to-the-rules/#the-data-source-column) from the procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) that checks that the _"data_source"_ column is always _'fb-survey'_.

[Here is the code](../check-data-conforms-to-the-rules/#the-geo-type-column) from the procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) that checks that the _"geo_type"_ column is always _'state'_.

## The meanings of the columns inform the design of the "covidcast_fb_survey_results" table

The descriptions above, and the successful outcome of the checks that the procedure [_"assert_assumptions_ok()"_](../ingest-scripts/cr-assert-assumptions-ok-sql) implements, inform the design of a single table to hold all of the data for the linear regression analysis.

The information in the following four columns is redundant and needn't be included in the projection: 

```
GEO_TYPE      # always 'state'
DATA_SOURCE   # always 'fb-survey'
CODE          # one-to-one with GEO_VALUE

SIGNAL        # will determine the destination column for VALUE and SAMPLE_SIZE
```

Further, the information in the following three columns isn't relevant for the linear regression analysis that this case study performs and also, therefore, needn't be included in the projection:

```
DIRECTION    # always NULL
ISSUE        # ISSUE and LAG are just metadata
LAG
```

Only these five columns remain of interest:

```
TIME_VALUE   # TIME_VALUE is the SURVEY_DATE. GEO_VALUE is the two-letter STATE code.
GEO_VALUE    # The (SURVEY_DATE, STATE) tuple is the primary key.

VALUE        # Will go to MASK_WEARING_PCT,          SYMPTOMS_PCT,          or CMNTY_SYMPTOMS_PCT.
STDERR       # Will go to MASK_WEARING_STDERR,       SYMPTOMS_STDERR,       or CMNTY_SYMPTOMS_STDERR.
SAMPLE_SIZE  # Will go to MASK_WEARING_SAMPLE_SIZE,  SYMPTOMS_SAMPLE_SIZE,  or CMNTY_SYMPTOMS_SAMPLE_SIZE.
```

This leads to the following design for the _"covidcast_fb_survey_results"_ table:

```
survey_date                 date     not null } primary
state                       text     not null }   key
mask_wearing_pct            numeric  not null
mask_wearing_stderr         numeric  not null
mask_wearing_sample_size    integer  not null
symptoms_pct                numeric  not null
symptoms_stderr             numeric  not null
symptoms_sample_size        integer  not null
cmnty_symptoms_pct          numeric  not null
cmnty_symptoms_stderr       numeric  not null
cmnty_symptoms_sample_size  integer  not null
```

The columns _"mask_wearing_pct"_,  _"symptoms_pct"_, and  _"cmnty_symptoms_pct"_ will have check constraints to ensure that the values are plausible percentages. And the columns _"mask_wearing_sample_size"_, _"symptoms_sample_size"_, _"cmnty_symptoms_sample_size"_,  _"mask_wearing_stderr"_,  _"symptoms_stderr"_, and _"cmnty_symptoms_stderr"_ will have check constraints to ensure that the values are positive. (The _"sample size"_ and _"stderr"_ columns are not used in the regression analysis.)
