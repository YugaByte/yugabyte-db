---
title: Computing Bacon Numbers for data form the IMDb
headerTitle: Computing Bacon Numbers for real IMDb data
linkTitle: Bacon numbers for IMDb data
description: This section shows how to compute Bacon Numbers for real IMDb data.
menu:
  latest:
    identifier: imdb-data
    parent: bacon-numbers
    weight: 20
isTocNested: true
showAsideToc: true
---

{{< tip title="Download a zip of scripts that include all the code examples that implement this case study" >}}

All of the `.sql` scripts that this section presents for copy-and-paste at the `ysqlsh` prompt are included for download in a zip-file.

[Download `recursive-with-case-studies.zip`](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/recursive-with-case-studies/recursive-with-case-studies.zip).

After unzipping it on a convenient new directory, you'll see a `README.txt`.  It tells you how to start the master-script that invokes all the child scripts that jointly instantiate the small and then the large IMDb data sets and compute the Bacon Numbers on these. Simply start it in `ysqlsh`. You can run it time and again. It always finishes silently. You can see the report that it produces on a dedicated spool directory and confirm that your reports are identical to the reference copies that are delivered in the zip-file.
{{< /tip >}}

## Download and ingest some IMDb data

Before trying the code in this section, make sure that you have created the supporting infrastructure:

- The _"actors"_, _"movies_", and _"cast_members"_ tables for representing IMDb data—see [`cr-actors-movies-cast-members-tables.sql`](../../bacon-numbers#cr-actors-movies-cast-members-tables-sql)

- The _"edges"_ table and the procedure to populate it from the _"cast_members"_ table—see [`cr-actors-movies-edges-table-and-proc-sql`](../../bacon-numbers#cr-actors-movies-edges-table-and-proc-sql)

- All the code shown in the section [Common code for traversing all kinds of graph](../../traversing-general-graphs/common-code/). Be sure to chose the [cr-raw-paths-no-tracing.sql](../../traversing-general-graphs/common-code#cr-raw-paths-no-tracing-sql) option.

Use Internet search for "download IMDb data for Bacon Numbers". This brings you to useful subsets of the vast, entire, IMDb data set that have been prepared by, for example, university Computer Science departments specifically to support Bacon Numbers course assignments. The [Oberlin College Computer Science](https://www.cs.oberlin.edu/) department is a good source. Go to [CSCI 151 Lab 10—Everything is better with Bacon](https://www.cs.oberlin.edu/~rhoyle/16f-cs151/lab10/index.html) and look for this:

> "[imdb.small.txt](http://cs.oberlin.edu/~gr151/imdb/imdb.small.txt): a 1817 line file with just a handful of performers (161), fully connected"

Here are a few lines from the file:

```
...
Alec Guinness|Kind Hearts and Coronets (1949)
Alec Guinness|Ladykillers, The (1955)
Alec Guinness|Last Holiday (1950)
Alec Guinness|Lavender Hill Mob, The (1951)
Alec Guinness|Lawrence of Arabia (1962)
Alec Guinness|Little Dorrit (1988)
Alec Guinness|Lovesick (1983)
...
```

After downloading it, rename it to `imdb-small.text` so that its name follows the same convention (a hyphen between the English words) that the all the other files that `recursive-with-case-studies.zip` contains. The information content is identical to what the _"cast_members"_ table was designed to represent. (See the [`cr-actors-movies-cast-members-tables.sql`](../../bacon-numbers#cr-actors-movies-cast-members-tables-sql) script.) This means that the `ysqlsh` metacommand `\COPY` can be used, straightforwardly, to ingest the downloaded data.

However, as this script creates it, the _"actors"_ and _"movies"_ tables must be populated first so that the foreign key constraints to these from the _"cast_members"_ table will be satisfied when it its populated. Use this script to do the steps in the proper order and display the resulting table counts:

##### `insert-imdb-data.sql`

```plpgsql
delete from edges;
delete from cast_members;
delete from actors;
delete from movies;

drop table if exists cast_members cascade;
create table cast_members(
  actor            text not null,
  movie            text not null,
  constraint imdb_facts_pk primary key(actor, movie));

\copy cast_members(actor, movie) from 'imdb-small.txt' with delimiter '|';

insert into actors select distinct actor from cast_members;
insert into movies select distinct movie from cast_members;

alter table cast_members
add constraint cast_members_fk1 foreign key (actor)
  references actors(actor)
  match full
  on delete cascade
  on update restrict;

alter table cast_members
add constraint cast_members_fk2 foreign key(movie)
  references movies(movie)
  match full
  on delete cascade
  on update restrict;

\t on
select 'count(*) from cast_members... '||to_char(count(*), '9,999') from cast_members;
select 'count(*) from actors......... '||to_char(count(*), '9,999') from actors;
select 'count(*) from movies......... '||to_char(count(*), '9,999') from movies;
\t off
```

This is the result:

```
 count(*) from cast_members...  1,817

 count(*) from actors.........    161

 count(*) from movies.........  1,586
```

## Populate the "edges" table and inspect the result

Populate the _"edges"_ table and inspect a sample of the results:

##### `do-insert-edges.sql`

```plpgsql
call insert_edges();

with v(actor) as (
  select node_1 from edges
  union all
  select node_2 from edges)
select distinct actor from v order by 1
limit 10;

select distinct unnest(movies) as movie
from edges
order by 1
limit 10;

select
  node_1,
  node_2,
  replace(translate(movies::text, '{"}', ''), ',', ' > ') as movies
from edges
where node_1 < node_2
order by 1, 2
limit 10;

select
  node_1,
  node_2,
  replace(translate(movies::text, '{"}', ''), ',', ' > ') as movies
from edges
where node_1 > node_2
order by 2, 1
limit 10;
```

Here are the results of all the queries:

```
      actor       
------------------
 Adam Sandler (I)
 Al Pacino
 Alan Rickman
 Alec Guinness
 Alireza Shayegan
 Allen Covert
 Alyson Hannigan
 Amir Ashayeri
 Andy Dick
 Ann Pala

                       movie                        
----------------------------------------------------
 180: Christopher Nolan Interviews Al Pacino (2002)
 Actor's Notebook: Christopher Lee (2002)
 Adam Sandler Goes to Hell (2001)
 Airport '77 (1977)
 America's Sweethearts (2001)
 Austin Powers: International Man of Mystery (1997)
 Bandits (2001)
 Batman Begins (2005)
 Being John Malkovich (1999)
 Big Parade of Comedy, The (1964)

      node_1      |       node_2       |                                        movies                                         
------------------+--------------------+---------------------------------------------------------------------------------------
 Adam Sandler (I) | Allen Covert       | Adam Sandler Goes to Hell (2001)
 Adam Sandler (I) | Ann Pala           | Adam Sandler Goes to Hell (2001)
 Adam Sandler (I) | Betsy Asher Hall   | Adam Sandler Goes to Hell (2001)
 Adam Sandler (I) | Bill Murray (I)    | Saturday Night Live Christmas (1999) > Saturday Night Live: Game Show Parodies (1998)
 Adam Sandler (I) | Billy Bob Thornton | Going Overboard (1989)
 Adam Sandler (I) | Billy Crystal      | Saturday Night Live: Game Show Parodies (1998)
 Adam Sandler (I) | Blake Clark        | Adam Sandler Goes to Hell (2001)
 Adam Sandler (I) | Craig A. Mumma     | Adam Sandler Goes to Hell (2001)
 Adam Sandler (I) | Dana Carvey        | Adam Sandler Goes to Hell (2001)
 Adam Sandler (I) | David Sosalla      | Adam Sandler Goes to Hell (2001)

       node_1       |      node_2      |                                        movies                                         
--------------------+------------------+---------------------------------------------------------------------------------------
 Allen Covert       | Adam Sandler (I) | Adam Sandler Goes to Hell (2001)
 Ann Pala           | Adam Sandler (I) | Adam Sandler Goes to Hell (2001)
 Betsy Asher Hall   | Adam Sandler (I) | Adam Sandler Goes to Hell (2001)
 Bill Murray (I)    | Adam Sandler (I) | Saturday Night Live Christmas (1999) > Saturday Night Live: Game Show Parodies (1998)
 Billy Bob Thornton | Adam Sandler (I) | Going Overboard (1989)
 Billy Crystal      | Adam Sandler (I) | Saturday Night Live: Game Show Parodies (1998)
 Blake Clark        | Adam Sandler (I) | Adam Sandler Goes to Hell (2001)
 Craig A. Mumma     | Adam Sandler (I) | Adam Sandler Goes to Hell (2001)
 Dana Carvey        | Adam Sandler (I) | Adam Sandler Goes to Hell (2001)
 David Sosalla      | Adam Sandler (I) | Adam Sandler Goes to Hell (2001)
```

## Find the paths

Invoke the implementation of "_find_paths()"_ that will be able to handle the real IMDd data that is shown at [`cr-find-paths-with-pruning.sql`](../../traversing-general-graphs/undirected-cyclic-graph/#cr-find-paths-with-pruning-sql). Invoke it _with_ early pruning:

```plpgsql
call find_paths('Kevin Bacon (I)', true);
```
Count the total number of paths that were found:

```plpgsql
\t on
select 'total number of pruned paths: '||count(*)::text from raw_paths;
\t off
```

This is the result.

```
total number of pruned paths: 160
```

You might like to use _"list_paths)"_ to list them all.

See what effect _"restrict_to_unq_containing_paths()"_ has:

```plpgsql
call restrict_to_unq_containing_paths('raw_paths', 'unq_containing_paths');
\t on
select 'total number of unique containing paths: '||count(*)::text from unq_containing_paths;
\t off
```
This is the result.

```
 total number of unique containing paths: 144
```
## Decorate the path edges with the list of movies that brought each edge

Ensure that you have created the _"decorated_paths_report()"_ table function using the [`cr-decorated-paths-report.sql`](../../bacon-numbers/#cr-decorated-paths-report-sql) script. Invoke it thus to list all five decorated shortest paths:

```plpgsql
\t on
select t from decorated_paths_report('raw_paths', 'Christopher Nolan');
\t off
```

This is the result:

```
 --------------------------------------------------
 Kevin Bacon (I)
    She's Having a Baby (1988)
    Wild Things (1998)
       Bill Murray (I)
          Saturday Night Live: Game Show Parodies (1998)
             Billy Crystal
                Muhammad Ali: Through the Eyes of the World (2001)
                   James Earl Jones
                      Looking for Richard (1996)
                         Al Pacino
                            180: Christopher Nolan Interviews Al Pacino (2002)
                               Christopher Nolan
 --------------------------------------------------
```
