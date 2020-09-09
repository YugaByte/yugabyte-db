---
title: Strings and text in YSQL
headerTitle: Strings and text
linkTitle: 8. Strings and text
description: Learn how to work with string and text data types in YSQL.
block_indexing: true
menu:
  v2.1:
    identifier: strings-and-text-1-ysql
    parent: learn
    weight: 570
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/develop/learn/strings-and-text-ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/develop/learn/strings-and-text-ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

## Introduction

Strings, character data types, or text. What you want to call it is up to you. Manipulating and outputting text is a very important topic that will be required for many different types of systems that you work with. The YugabyteDB SQL API offers extensive text capability that will be demonstrated here.

## About character data types

### Character data types

For character data types, see [Data types](/latest/api/ysql/datatypes/). Note that YugabyteDB implements the data type aliases and that is what is used here.

With PostgreSQL, the use of different character data types has a historical aspect. YugabyteDB — being a more recent implementation — has no such history. Consider keeping your use of character data types simple, ideally just 'text', or 'varchar(n)' if you require a restricted length. Although it's your choice, using text and then verifying the length of a character string will allow you to develop your own approach to managing this scenario, rather than encountering errors by exceeding some arbitrary length.

{{< note title="Note" >}}
If you use char(n), character(n), or varchar(n), then the limitation will be the number you assign, which cannot exceed 10,485,760. For unlimited length, use a character data type without a length description, such as 'text'. However, if you have specific requirements to ignore trailing spaces, then you may wish to consider using char(n).
{{< /note >}}

The following example shows a few ways to work with the different data types.

```
./bin/ysqlsh

ysqlsh (11.2)
Type "help" for help.

yugabyte=# create table text_columns(a_text text, a_varchar varchar, a_char char, b_varchar varchar(10), b_char char(10));

CREATE TABLE

yugabyte=# insert into text_columns values('abc ', 'abc ', 'abc ', 'abc ', 'abc ');
ERROR:  value too long for type character(1)

yugabyte=# insert into text_columns values('abc ', 'abc ', 'a', 'abc ', 'abc ');
INSERT 0 1

yugabyte=# select * from text_columns
           where a_text like 'ab__' and a_varchar like 'ab__'
           and b_varchar like 'ab__';

 a_text | a_varchar | a_char | b_varchar |   b_char
--------+-----------+--------+-----------+------------
 abc    | abc       | a      | abc       | abc

yugabyte=# select * from text_columns
           where a_text like 'ab__' and a_varchar like 'ab__'
           and b_varchar like 'ab__' and b_char like 'ab__';

 a_text | a_varchar | a_char | b_varchar | b_char
--------+-----------+--------+-----------+--------
(0 rows)

yugabyte=# select length(a_text) as a_text, length(a_varchar) as a_varchar, length(a_char) as a_char,
           length(b_varchar) as b_varchar, length(b_char) as b_char
           from text_columns;

 a_text | a_varchar | a_char | b_varchar | b_char
--------+-----------+--------+-----------+--------
      4 |         4 |      1 |         4 |      3
```

In the example above, notice that the column `b_char` does not contain a trailing space and this could impact your SQL. And, if you specify a maximum length on the column definition, the SQL can also generate errors, so you will have to either manually truncate your input values or introduce error handling.

### Casting

When you are working with text that has been entered by users through an application, ensure that YugabyteDB understands that it is working with a text input. All values should be cast unless they can be trusted due to other validation measures that have already occurred.

Start YSQL and you can see the impacts of casting.

```sh
./bin/ysqlsh

ysqlsh (11.2)
Type "help" for help.

yugabyte=# select cast(123 AS TEXT), cast('123' AS TEXT), 123::text, '123'::text;

 text | text | text | text
------+------+------+------
 123  | 123  | 123  | 123

yugabyte=# select tablename, hasindexes AS nocast, hasindexes::text AS casted
  from pg_catalog.pg_tables
  where tablename in('pg_default_acl', 'sql_features');
  
   tablename    | nocast | casted
----------------+--------+--------
 pg_default_acl | t      | true
 sql_features   | f      | false
```

In the last example above, the column 'hasindexes' is a `Boolean` data type and by casting it to `text`, you will receive a text result of `true` or `false`.

## Manipulating text

There are a lot of functions that can be applied to text. Below the functions are classified into logical groupings - in many cases the capability of the functions overlap and personal choice will determine how you approach solving the problem.

The focus here was to quickly show how each of the functions could be used, along with some examples. It is assumed that you have the [`yb_demo` database](/latest/quick-start/explore-ysql/#1-load-data) installed.

### Altering the appearance of text

```sh
yugabyte=# \c yb_demo  
You are now connected to database "yb_demo" as user "yugabyte".

yb_demo =# select lower('hELLO world') AS LOWER,
  upper('hELLO world') AS UPPER,
  initcap('hELLO world') AS INITCAP;

    lower    |    upper    |   initcap
-------------+-------------+-------------
 hello world | HELLO WORLD | Hello World

yb_demo =# select quote_ident('ok') AS EASY, quote_ident('I am OK') AS QUOTED, quote_ident('I''m not OK') AS DOUBLE_QUOTED, quote_ident('') AS EMPTY_STR, quote_ident(null) AS NULL_QUOTED;

 easy |  quoted   | double_quoted | empty_str | null_quoted
------+-----------+---------------+-----------+-------------
 ok   | "I am OK" | "I'm not OK"  | ""        |

yb_demo =# select quote_literal('ok') AS EASY, quote_literal('I am OK') AS QUOTED, quote_literal('I''m not OK') AS DOUBLE_QUOTED, quote_literal('') AS EMPTY_STR, quote_literal(null) AS NULL_QUOTED;

 easy |  quoted   | double_quoted | empty_str | null_quoted
------+-----------+---------------+-----------+-------------
 'ok' | 'I am OK' | 'I''m not OK' | ''        |

yb_demo =# select quote_nullable('ok') AS EASY, quote_nullable('I am OK') AS QUOTED, quote_nullable('I''m not OK') AS DOUBLE_QUOTED, quote_nullable('') AS EMPTY_STR, quote_nullable(null) AS NULL_QUOTED;

easy |  quoted   | double_quoted | empty_str | null_quoted
------+-----------+---------------+-----------+-------------
 'ok' | 'I am OK' | 'I''m not OK' | ''        | NULL
```

Use `quote_ident` to parse identifiers in SQL like column names and `quote_nullable` as a string literal that may also be a null.

You can use "dollar sign quoting" to parse raw text — any text contained within dollar sign quotations are treated as a raw literal. The starting and ending markers do not need to be identical, but must start and end with a dollar sign (`$`). See the examples below.

```
yugabyte=# select $$%&*$&$%7'\67458\''""""';;'\//\/\/\""'/'''''"""""'''''''''$$;

                         ?column?
-----------------------------------------------------------
 %&*$&$%7'\67458\''""""';;'\//\/\/\""'/'''''"""""'''''''''

yugabyte=# select $__unique_$           Lots of space
yugabyte=#                    and multi-line too       $__unique_$;

                   ?column?
----------------------------------------------
            Lots of space                    +
                    and multi-line too

yugabyte=# select $$first$$ AS "F1", $$second$$ AS "F2";

  F1   |   F2
-------+--------
 first | second
```

Some values need to be padded for formatting purposes, and `LPAD` and `RPAD` are meant for this purpose. They mean 'left pad' and 'right pad' respectively. They are normally used to fill with spaces but you could specify anything, including more than a single character. So you could pad with underscores (`_`) or spaced dots `. . .`, or anything you wish. You do not specify how much to pad, but the maximum length to pad. Therefore, if your value is already as long as your maximum length, then no padding is required. Note that this can cause a truncation if your field is longer than the maximum length specified.

The reverse of padding is trimming, which will remove spaces if found. Below are examples of using padding and trimming to achieve the results required.

```
yb_demo=# select name, lpad(name, 10), rpad(name, 15) from users order by name limit 5;

       name        |    lpad    |      rpad
-------------------+------------+-----------------
 Aaron Hand        | Aaron Hand | Aaron Hand
 Abbey Satterfield | Abbey Satt | Abbey Satterfie
 Abbie Parisian    | Abbie Pari | Abbie Parisian
 Abbie Ryan        | Abbie Ryan | Abbie Ryan
 Abby Larkin       | Abby Larki | Abby Larkin

yb_demo=# select name, lpad(name, 20), rpad(name, 20) from users order by name limit 5;

       name        |         lpad         |         rpad
-------------------+----------------------+----------------------
 Aaron Hand        |           Aaron Hand | Aaron Hand
 Abbey Satterfield |    Abbey Satterfield | Abbey Satterfield
 Abbie Parisian    |       Abbie Parisian | Abbie Parisian
 Abbie Ryan        |           Abbie Ryan | Abbie Ryan
 Abby Larkin       |          Abby Larkin | Abby Larkin

yb_demo=# select name, lpad(name, 20, '. '), rpad(name, 20, '.') from users order by name limit 5;

       name        |         lpad         |         rpad
-------------------+----------------------+----------------------
 Aaron Hand        | . . . . . Aaron Hand | Aaron Hand..........
 Abbey Satterfield | . .Abbey Satterfield | Abbey Satterfield...
 Abbie Parisian    | . . . Abbie Parisian | Abbie Parisian......
 Abbie Ryan        | . . . . . Abbie Ryan | Abbie Ryan..........
 Abby Larkin       | . . . . .Abby Larkin | Abby Larkin.........

yb_demo=# select repeat(' ', ((x.maxlen-length(u.name))/2)::int) || rpad(u.name, x.maxlen) AS "cname"
          from users u,
          (select max(length(a.name))::int AS maxlen from users a) AS x;

            cname
------------------------------
      Stewart Marks
      Regan Corkery
    Domenic Daugherty
    Winfield Donnelly
    Theresa Kertzmann
    Terrence Emmerich
      Hudson Jacobi
      Aidan Hagenes
    Virgil Schowalter
      Rahul Kreiger
    Wilhelmine Erdman
      Elwin Okuneva
  Maximillian Dickinson  
      Lucie Cormier
  Alexandrine Rosenbaum  
    Jayne Breitenberg
  Alexandria Schowalter  
 Augustine Runolfsdottir
    Mathilde Weissnat
      Theresa Grant
 ...

yb_demo=# select x.RawDay, length(x.RawDay) AS RawLen, x.TrimDay, length(x.TrimDay) AS TrimLen,
          x.LTrimDay, length(x.LTrimDay) AS LTrimLen, x.RTrimDay, length(x.RTrimDay) AS RTrimLen
          from (select to_char(generate_series, 'Day') AS RawDay,
                trim(to_char(generate_series, 'Day')) AS TrimDay,
                ltrim(to_char(generate_series, 'Day')) AS LTrimDay,
                rtrim(to_char(generate_series, 'Day')) AS RTrimDay  
                from generate_series(current_date, current_date+6, '1 day')) AS x;

  rawday   | rawlen |  trimday  | trimlen | ltrimday  | ltrimlen | rtrimday  | rtrimlen
-----------+--------+-----------+---------+-----------+----------+-----------+----------
 Wednesday |      9 | Wednesday |       9 | Wednesday |        9 | Wednesday |        9
 Thursday  |      9 | Thursday  |       8 | Thursday  |        9 | Thursday  |        8
 Friday    |      9 | Friday    |       6 | Friday    |        9 | Friday    |        6
 Saturday  |      9 | Saturday  |       8 | Saturday  |        9 | Saturday  |        8
 Sunday    |      9 | Sunday    |       6 | Sunday    |        9 | Sunday    |        6
 Monday    |      9 | Monday    |       6 | Monday    |        9 | Monday    |        6
 Tuesday   |      9 | Tuesday   |       7 | Tuesday   |        9 | Tuesday   |        7
```

The final padding example above shows how you can center text and the trim example shows the impacts of the different trims on a value that is padded. Note that the 'Day' value is right-padded to 9 characters which is why a left-trim has no impact upon the field length at all, only the right-trim or a 'full' trim will remove spaces.

You can also state that a text value is 'escaped' by prefixing with an 'e' or 'E'. Take a look at this example.

```
yugabyte=# select E'I''ve told YugabyteDB that this is an escaped string\n\tso I can specify escapes safely' as escaped_text;

                   escaped_text
---------------------------------------------------
 I've told YugabyteDB that this is an escaped string+
         so I can specify escapes safely

yugabyte=# select E'a\\b/c\u00B6' as escaped_txt, 'a\\b/c\u00B6' as raw_txt;

 escaped_txt |   raw_txt
-------------+--------------
 a\b/c¶     | a\\b/c\u00B6
```

{{< note title="Note" >}}
`\n` refers to a new line, and `\t` is a tab, hence the formatted result.
{{< /note >}}

YugabyteDB also has `DECODE` and `ENCODE` for decoding and encoding from, or to, binary data. It caters for 'base64', 'hex' and 'escape' representations. Decode will give the output in `BYTEA` data type. Additionally, you can use the `TO_HEX` command to convert an ascii number to its digital representation.

#### Joining strings

You can concatenate strings of text in several different ways. For robustness, you should ensure that everything being passed is interpreted as text (by casting) so that unexpected results do not appear in edge cases. Here are some examples that show that YugabyteDB is leniant in passing in variables, but you should implement more robust casting for proper treatment of strings.

```sh
yb_demo=# select 'one' || '-' || 2 || '-one' AS "121";

    121
-----------
 one-2-one

yb_demo=# select 2 || '-one-one' AS "211";

    211
-----------
 2-one-one

yb_demo=# select 1 || '-one' || repeat('-two', 2) AS "1122";

     1122
---------------
 1-one-two-two

yb_demo=# select 1::text || 2::text || 3::text AS "123";

 123
-----
 123

yb_demo=# select 1 || 2 || 3 AS "123";

ERROR:  operator does not exist: integer || integer
LINE 1: select 1 || 2 || 3 AS "123";
                 ^
HINT:  No operator matches the given name and argument types. You might need to add explicit type casts.

yb_demo=# select concat(1,2,3) AS "123";

 123
-----
 123

yb_demo=# select concat_ws(':', 1,2,3) AS "123 WS";

 123 WS
--------
 1:2:3
(1 row)

yb_demo =# select left(vendor,1) AS V, string_agg(distinct(category), ', ' ORDER BY category) AS CATEGORIES
  from products group by left(vendor,1) order by 1;
  
 v |            categories
---+----------------------------------
 A | Doohickey, Gadget, Gizmo
 B | Doohickey, Gadget, Gizmo, Widget
 C | Doohickey, Gadget, Gizmo, Widget
 D | Gadget, Gizmo, Widget
 E | Gadget, Gizmo, Widget
 F | Doohickey, Gadget, Gizmo, Widget
 G | Doohickey, Gadget, Widget
 H | Doohickey, Gadget, Gizmo, Widget
 I | Gizmo, Widget
 J | Doohickey, Gadget, Gizmo, Widget
 K | Doohickey, Gadget, Gizmo, Widget
 L | Doohickey, Gadget, Gizmo, Widget
 M | Doohickey, Gadget, Gizmo, Widget
 N | Doohickey, Gadget, Widget
 O | Doohickey, Gadget, Gizmo, Widget
 P | Doohickey, Gadget, Gizmo, Widget
 Q | Doohickey
 R | Doohickey, Gadget, Gizmo, Widget
 S | Doohickey, Gadget, Gizmo, Widget
 T | Gizmo, Widget
 U | Gadget
 V | Doohickey, Widget
 W | Doohickey, Gadget, Gizmo, Widget
 Z | Gizmo
```

In the example above, you explore the `LEFT` function, but the `string_agg` function is best used by an input of a series or a set of data as done in SQL rows. The example shows how the aggregated string has its own order by compared to the outer SQL which is the vendors being classified A-Z.

There is also the `REVERSE` function that reverses the contents of text in a simple manner as shown in the next example.

```sh
yb_demo=# select reverse(to_char(current_date, 'DD-MON-YYYY'));

   reverse
-------------
 9102-LUJ-92
```

You can use the `FORMAT` function parse user input as parameters to a SQL statement in order to minimise the impact of unexpected data that is typical of a SQL injection attack. The most popular method is to use the `EXECUTE` command within a procedure as this is not available at the YSQL command prompt, only within the YSQL plpgsql environment. The `FORMAT` command is used to finalise the complete SQL statement and passed to `EXECUTE` to run. As you are not simulating YSQL plpgsql here, let's illustrate how to use the `FORMAT` function only.

```
yb_demo=# select format('Hello %s, today''s date is %s', 'Jono', to_char(current_date, 'DD-MON-YYYY'), 'discarded');

                 format
-----------------------------------------
 Hello Jono, today's date is 29-JUL-2019

yb_demo=# select format('On this day, %2$s, %1$s was here', 'Jono', to_char(current_date, 'DD-MON-YYYY'));

                 format
-----------------------------------------
 On this day, 29-JUL-2019, Jono was here

yb_demo=# select format('SELECT %2$I, %3$I from %1$I where name = %4$L', 'users', 'birth_date', 'email', 'Brody O''Reilly');

                               format
--------------------------------------------------------------------
 SELECT birth_date, email from users where name = 'Brody O''Reilly'

```

#### Substituting text

Substituting text with other text can be a complex task as you need to fully understand the scope of the data that the functions can be subject to. A common occurrence is failure due to an unexpected value being passed through, like `NULL`, an empty string `''`, or a value that YugabyteDB would interpret as a different data type like `true` or `3`.

The treatment of nulls in mathematical operations is often problematic, as is string joins as joining a null to a value results in a null. Coalescing the inputs will avoid these issues as shown in the examples below.

```
yb_demo=# select trunc(avg(coalesce(discount,0))::numeric,3) AS "COALESCED", trunc(avg(discount)::numeric,3) AS "RAW" from orders;

 COALESCED |  RAW  
-----------+-------
     0.530 | 5.195

yb_demo=# select 'Hello ' || null AS GREETING, 'Goodbye ' || coalesce(null, 'Valued Customer') AS GOODBYE;

 greeting |         goodbye
----------+-------------------------
          | Goodbye Valued Customer
```

The above shows how substituting when null can have a significant impact upon the results you achieve or even the behaviour of your application. Below concentrates on changing existing text with other text.

```
yb_demo=# select overlay(password placing 'XXXXXXXXXXXXXXX' from 1 for length(password)) AS SCRAMBLED from users limit 5;

    scrambled
-----------------
 XXXXXXXXXXXXXXX
 XXXXXXXXXXXXXXX
 XXXXXXXXXXXXXXX
 XXXXXXXXXXXXXXX
 XXXXXXXXXXXXXXX

yb_demo=# select regexp_replace('Hi my number is +999 9996-1234','[[:alpha:]]','','g');

   regexp_replace
--------------------
     +999 9996-1234

yb_demo=# select 'I think I can hear an ' || repeat('echo.. ', 3) AS CLICHE;

                   cliche
---------------------------------------------
 I think I can hear an echo.. echo.. echo..

yb_demo=# select replace('Gees I love Windows', 'Windows', 'Linux') AS OBVIOUS;

      obvious
-------------------
 Gees I love Linux
```

The `REGEXP_REPLACE` function along with the other REGEX functions require an entire chapter to themselves with the sophistication that can be achieved - which is well beyond this scope of this introductory walk through. The example above strips out all characters of the alphabet and replaces them with an empty string. The 'g' flag is 'global' that results in the replace to occur throughout the entire string, without the 'g' flag the replace will stop after the first substitution. Note that the result contains spaces which is why it appears odd. You might think that this example shows an extraction of non-alphabetical characters, but it is just replacing them with an empty string.

#### Extracting text

There are several ways of extracting text from text, in some cases it might be part of 'cleaning' the text, note that removing leading or trailing spaces is covered by the trim functions shown above. The remaining functions here show how parts of text can be manipulated.

```
yb_demo=# select left('123456', 3);

 left
------
 123

yb_demo=# select right('123456', 3);

 right
-------
 456

yb_demo=# select substr('123456', 3);

 substr
--------
 3456

yb_demo=# select substr('123456', 3, 2);

 substr
--------
 34

yb_demo=# select substr('123456', position('4' in '123456')+1, 2);

 substr
--------
 56

yb_demo=# select substring('123456', position('4' in '123456')+1, 2);

 substring
-----------
 56

yb_demo=# select replace(substr(email, position('@' in email)+1, (length(email)
            -position('.' in substr(email, position('@' in email)+1)))), '.com', '') AS "Domain", count(*)
          from users
          group by 1;

 Domain  | count
---------+-------
 hotmail |   813
 yahoo   |   838
 gmail   |   849

```

{{< note title="Note" >}}
The command ```SUBSTRING``` has overloaded equivalents that accept POSIX expressions. The above example shows you the simple use of ```SUBSTRING``` which can also be used as ```SUBSTR```. therefore it is recommended to only use the full ```SUBSTRING``` command when using POSIX.
{{< /note >}}

As stated above for ```REGEXP_REPLACE```, the full explanation of regular expressions requires its own comprehensive documentation that is not covered here. Here is an example illustrating its use.

```
yb_demo=# select name as Fullname, regexp_match(name, '(.*)(\s+)(.*)') AS "REGEXED Name",
          (regexp_match(name, '(.*)(\s+)(.*)'))[1] AS "First Name",
          (regexp_match(name, '(.*)(\s+)(.*)'))[3] AS "Last Name"
          from users limit 5;

    fullname    |     REGEXED Name     | First Name | Last Name
----------------+----------------------+------------+-----------
 Jacinthe Rowe  | {Jacinthe," ",Rowe}  | Jacinthe   | Rowe
 Walter Mueller | {Walter," ",Mueller} | Walter     | Mueller
 Fatima Murphy  | {Fatima," ",Murphy}  | Fatima     | Murphy
 Paxton Mayer   | {Paxton," ",Mayer}   | Paxton     | Mayer
 Mellie Wolf    | {Mellie," ",Wolf}    | Mellie     | Wolf
```

{{< note title="Note" >}}
In the example abov, you are asking the 'name' column to be segmented by the existence of a space (`\s`) and then reporting the first and third set of text reported by the match. The regular expression returns a text array, not a text value, and thus you have to reference the array index to access the value as text. Note that this SQL would be very vulnerable to errors caused by data entry, including a middle name or missing either a first or last name would cause errors.
{{< /note >}}

Now, let's look at some manipulation and splitting of text so that you can process it in pieces. For this example, I will be using a sample extract from a bank file that is used for processing payments. This example could apply if the entire file was uploaded as a single text entry into a table and you select it and then process it.

```
yb_demo=# create table bank_payments(bank_file text);

CREATE TABLE

yb_demo=# insert into bank_payments values($$"CMGB","1.0","95012141352105","999999","30128193018492","20","","GBP","B","Beneficiary name18","Txt on senders acc","Txt for credit acc","","","","","","909170/1","AB"
"CMGB","1.0","95012141352105","999999","95012113864863","10.00","","GBP","B","Beneficiary name18","Txt on senders acc","Txt for credit acc","","","","","Remitters name  18","Tech ref for automatic processing5","AT","/t.x",
"CMGB","1.0","95012141352105","","30128193018492","21","","GBP","C","Beneficiary name18","Txt on senders acc","","Txt for credit acc","","","","","909175/0","AB"
"CMGB","1.0","95012141352105","","30128193018492","22","","GBP","I","Beneficiary name18","Txt on senders acc","text","","","","","","909175/1","AB"
"CMGB","1.0","95012141352105","","30128193018492","23","","GBP","F","Beneficiary name18","Txt on senders acc","Txt for credit acc","","","","","","909171/0","AB"$$);

INSERT 0 1

yb_demo=# select regexp_split_to_table(bank_file, chr(10)) from bank_payments;

                                                                                                     regexp_split_to_table
--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 "CMGB","1.0","95012141352105","999999","30128193018492","20","","GBP","B","Beneficiary name18","Txt on senders acc","Txt for credit acc","","","","","","909170/1","AB"
 "CMGB","1.0","95012141352105","999999","95012113864863","10.00","","GBP","B","Beneficiary name18","Txt on senders acc","Txt for credit acc","","","","","Remitters name  18","Tech ref for automatic processing5","AT","/t.x",
 "CMGB","1.0","95012141352105","","30128193018492","21","","GBP","C","Beneficiary name18","Txt on senders acc","","Txt for credit acc","","","","","909175/0","AB"
 "CMGB","1.0","95012141352105","","30128193018492","22","","GBP","I","Beneficiary name18","Txt on senders acc","text","","","","","","909175/1","AB"
 "CMGB","1.0","95012141352105","","30128193018492","23","","GBP","F","Beneficiary name18","Txt on senders acc","Txt for credit acc","","","","","","909171/0","AB"

yb_demo=# select split_part(f.line, ',' , 8) AS "currency",
                 split_part(f.line, ',' , 5) AS "Account"
                 from (select regexp_split_to_table(bank_file, chr(10)) AS "line" from bank_payments) AS f;

 currency |     Account
----------+------------------
 "GBP"    | "30128193018492"
 "GBP"    | "95012113864863"
 "GBP"    | "30128193018492"
 "GBP"    | "30128193018492"
 "GBP"    | "30128193018492"
```

_Remember to drop the table 'bank_payments' if it is no longer required._

```
yb_demo=# select reverse(translate(replace(lower(i.input), ' ', ''),
                         'abcdefghijklmnopqrstuvwxyz',
                         'A8Cd349h172!mN0pQr$TuVw*yZ')) AS "simplePWD"
          from (select 'type a word here' AS "input") AS i;

   simplePWD
---------------
 3r3hdr0wA3pyT
```

The `TRANSLATE` command above will replace multiple different characters in a single command which can be useful. In the example above, the 'a' is replaced with a 'A', and 'b' is replaced with the number '8', and so forth.

### Obtaining information of text

Rather than format or change the contents of text, you often might want to understand particular attributes of the text. Below are some examples of using commands to return information of the text.

```
yb_demo=# select x.c AS CHAR, ascii(x.c) AS ASCII
          from (select regexp_split_to_table(i.input, '') AS "c"
                from (select 'hello' AS input) AS i) AS x;
  
 char | ascii
------+-------
 h    |   104
 e    |   101
 l    |   108
 l    |   108
 o    |   111

yb_demo=# select bit_length('hello'), char_length('hello'), octet_length('hello');

 bit_length | char_length | octet_length
------------+-------------+--------------
         40 |           5 |            5

yb_demo=# select array_agg(chr(ascii(x.c))) AS "CHAR"
          from (select regexp_split_to_table(i.input, '') AS "c"
                from (select 'hello' AS input) AS i) AS x;
  
    CHAR
-------------
 {h,e,l,l,o}

yb_demo=# select avg(length(name))::int AS AVG_LENGTH from users;

 avg_length
------------
         14

yb_demo=# select name from users
          where position('T' in name) > 2
          and position('p' in name) = length(name)
          order by name;

      name
-----------------
 Cory Tromp
 Demario Tromp
 Demetris Tromp
 Deon Tromp
 Emelia Tromp
 Ivah Tromp
 Jany Torp
 Jared Tromp
 Judd Tromp
 Larue Torp
 Magdalen Torp
 Margarita Tromp
 Marjolaine Torp
 Patrick Torp
 Porter Tromp
 Rebeka Tromp

yb_demo=# select name, position('ar' in name) AS posn, strpos(name, 'ar') as strpos
          from users
          where strpos(name, 'ark') > 0
          order by name desc limit 10;

      name      | posn | strpos
----------------+------+--------
 Yasmin Stark   |   10 |     10
 Veronica Stark |   12 |     12
 Tamia Larkin   |    8 |      8
 Stewart Marks  |    5 |      5
 Ryann Parker   |    8 |      8
 Rudy Larkin    |    7 |      7
 Rodolfo Larkin |   10 |     10
 Novella Marks  |   10 |     10
 Markus Hirthe  |    2 |      2
 Mark Klein     |    2 |      2

yb_demo=# select m.name
          from (select to_char(generate_series, 'Month') AS name
                from generate_series(current_date-364, current_date, '1 month')) AS m
          where starts_with(m.name, 'J');

   name
-----------
 January  
 June
 July  
```

## Something a bit more advanced

If you like a bit of a challenge, below is an example that URL escapes a string. There is still some more room for tweaking in its current form, that is left for you to do.

```
yugabyte=# select string_agg(case
                              when to_hex(ascii(x.arr::text))::text
                                   in('20','23','24','25','26','40','60','2b','2c','2f','3a','3b','3c','3d','3e','3f',
                                   '5b','5c','5d','5e','7b','7c','7d') then '%' || to_hex(ascii(x.arr::text))::text
                              else x.arr
                              end, '') AS "url_escaped"
           from (select regexp_split_to_table('www.url.com/form?name="My name"&dob="1/1/2000"&email="hello@example.com"', '')) AS x (arr);
                             url_escaped
------------------------------------------------------------------------------------------------
 www.url.com%2fform%3fname%3d"My%20name"%26dob%3d"1%2f1%2f2000"%26email%3d"hello%40example.com"
```

## Conclusion

Text or strings are part of every conceivable system. YugabyteDB provides you with comprehensive capabilities to manage and manipulate all your text within the database.
