---
title: Case study—using a WITH clause recursive substatement to compute Bacon Numbers on IMDb data
headerTitle: Case study—computing Bacon Numbers for actors listed in the IMDb
linkTitle: case study—Bacon Numbers from IMDb
description: Case study showing how to use a WITH clause recursive substatement to solve the "Six Degrees of Kevin Bacon" problem using IMDb data.
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: bacon-numbers
    parent: with-clause
    weight: 60
isTocNested: true
showAsideToc: true
---

The Bacon Numbers problem, sometimes referred to as "The Six_Degrees of Kevin Bacon" (see [this Wikipedia article](https://en.wikipedia.org/wiki/Six_Degrees_of_Kevin_Bacon)), is a specific formulation of the general problem of tracing paths in an undirected cyclic graph. It is a well-known set-piece exercise in graph analysis and is a popular assignment task in computer science courses. Most frequently, solutions are implemented in an "if-then-else" language like Java. Interestingly, solutions can be implemented in SQL and, as this section will show, the amount of SQL needed is remarkably small.

## Representing actors and movies data

The Bacon Numbers problem is conventionally formulated in the context of the data represented in the IMDb—an acronym for Internet Movie Database. See [this Wikipedia article](https://en.wikipedia.org/wiki/IMDb). The data are freely available from IMDb but it's better, for the purposes of this section's pedagogy, to use sufficient subsets of the total IMDb content. These subsets restrict the population to the movies in which Kevin Bacon has acted and project the facts about the actors and movies to just their names. This entity relationship diagram (a.k.a. ERD) depicts the sufficient subset of the IMDb: 

![imdb-erd](/images/api/ysql/the-sql-language/with-clause/bacon-numbers/imdb-erd.jpg)

- _each actor must act in at least one movie_
- _each movie's cast must list at least one actor_

The actors are the nodes in an undirected cyclic graph. There is an edge between two actors when the both have acted together in at least one movie.

The ERD implies the conventional three-table representation with an _"actors_ table, a _"movies_table"_, and a _"cast_members"_ intersection table. Create them with this script:

##### `cr-actors-movies-cast-members-tables.sql`

```plpgsql
drop table if exists actors cascade;
drop table if exists movies cascade;
drop table if exists cast_members cascade;

create table actors(
  actor            text primary key);

create table movies(movie text primary key);

create table cast_members(
  actor text not null,
  movie text not null,

  constraint cast_members_pk primary key(actor, movie),

  constraint cast_members_fk1 foreign key(actor)
    references actors(actor)
    match full
    on delete cascade
    on update restrict,

  constraint cast_members_fk2 foreign key(movie)
    references movies(movie)
    match full
    on delete cascade
    on update restrict
  );
```

Of course, the IMDb has facts like _date of birth_, _nationality_, and so on for the actors and like _release date_, _language_ and so on for the movies. The information would doubtless allow the _"cast_members"_ table to have columns like _"character_name"_. The data that this case study uses happen to include the movie release date, in parentheses, after the movie name in a single text field. The pedagogy is sufficiently served without parsing out these two facts into separate columns in the _"movies"_ table.

Notice that the notion of a graph is so far only implied. A derived _"edges"_ table makes the graph explicit.  An edge exists between a pair of actors if they are both on the cast list of one or more movies. The SQL needed to populate the _"edges"_ table from the _"cast_members"_ table is straightforward.

When the paths have been found, it's useful to be able to annotate each edge with the list of movies that are responsible for its existence. The annotation code could, of course, derive this information dynamically. But it simplifies the overall coding scheme if a denormalization is adopted to annotate the paths at the time that they are discovered. Another departure from strict purity simplifies the overall coding scheme further. If the row for the edge between a particular pair of actors records the _list_ of movies that brought it (rather than recording many edges, each with a single-valued _"movie"_ attribute), then the path-tracing code that the section [Using a `WITH` clause recursive substatement to traverse graphs of all kinds](../traversing-general-graphs/) presented can be used "as is". To this end, the columns that represent the actor pair in the _"edges"_ table are called _"node_1"_ and "_node_2"_ rather than the more natural _"actor_1"_ and _"actor_2"_.

This code creates the _"edges"_ table and the procedure that populates it.

##### `cr-actors-movies-edges-table-and-proc.sql`

```plpgsql
drop table if exists edges cascade;

create table edges(
  node_1 text,
  node_2 text,
  movies text[],
  constraint edges_pk primary key(node_1, node_2),
  constraint edges_fk_1 foreign key(node_1) references actors(actor),
  constraint edges_fk_2 foreign key(node_2) references actors(actor));

drop procedure if exists insert_edges() cascade;

create or replace procedure insert_edges()
  language plpgsql
as $body$
begin
  delete from edges;

  with
    v1(node_1, movie) as (
      select actor, movie from cast_members),

    v2(node_2, movie) as (
      select actor, movie from cast_members)

  insert into edges(node_1, node_2, movies)
  select node_1, node_2, array_agg(movie order by movie)
  from v1 inner join v2 using (movie)
  where node_1 < node_2
  group by node_1, node_2;

  insert into edges(node_1, node_2, movies)
  select node_2 as node_1, node_1 as node_2, movies
  from edges;
end;
$body$;
```

Notice the second `INSERT` statement that re-inserts all the discovered directed edges in the reverse direction. The value of this denormalization is explained in the section [Finding the paths in a general undirected cyclic graph](../traversing-general-graphs/undirected-cyclic-graph/).

## Create a stored procedure to decorate path edges with the list of movies that brought each edge

The stored procedure (actually a table function) will annotate each successive edge along each path in the specified table with the list of movies that brought that edge.

- When there are relatively few paths in all, as there are with the synthetic data that the section [Computing Bacon Numbers for a small set of synthetic actors and movies data](./synthetic-data/) uses, it's convenient simply to show all the decorated paths.

- However, with a data set as big as the IMDb (even the [imdb.small.txt](http://cs.oberlin.edu/~gr151/imdb/imdb.small.txt) subset has 160 shortest paths), it's useful to be able to name a candidate actor and to annotate just the shortest path (more carefully stated, one of the shortest paths) from Kevin Bacon to the candidate. The site [The Oracle of Bacon](https://oracleofbacon.org/movielinks.php) exposes this functionality.

The first formal parameter of the function  _"decorated_paths_report()"_ is mandatory and specifies the table in which the paths are represented. The second optional formal parameter, _"terminal"_, lets you specify the last node along a path. If you omit it, the meaning is _"report all the paths"_; and it you supply it, the meaning is _"report the path to the specified actor"_.

Dynamic SQL is therefore needed for two reasons, each of which alone is a sufficient reason:

- The table name isn't known until run-time.

- There may, or may not, be a `WHERE` clause.

##### `cr-decorated-paths-report.sql`

```plpgsql
drop function if exists decorated_paths_report(text, text) cascade;

-- This procedure is more elaborate than you'd expect because of GitHub Issue 3286.
-- It says this in the report:
--
--   Commit 9d66392 added support for cursor. Our next releases will have this work.
--   However, there are a few pending issues.
--
-- Meanwhile, this code works around the issue by using a single-row SELECT... INTO.
-- This is made possible by using array_agg(). But you cannot aggregate arrays of
-- different cardinalities. So a second-level workaround is used. Each array in
-- the result set is cast to "text" for aggregation and then cast back to the array
-- that it represents in the body of the FOREACH loop that steps through the text
-- values that have been aggregated.
--
-- When a "stable" release supports the use of a cursor variable, this implementation
-- will be replaced by a more straightforward version.

create function decorated_paths_report(tab in text, terminal in text default null)
  returns table(t text)
  language plpgsql
as $body$
<<b>>declare
  indent constant int  := 3;
  q      constant text := '''';

  stmt_start constant text := 'select array_agg((path::text) '||
                              'order by cardinality(path), terminal(path), path) '||
                              'from ?';

  where_ constant text := ' where terminal(path) = $1';

  all_terminals_stmt  constant text := replace(stmt_start, '?', tab);
  one_terminal_stmt   constant text := replace(stmt_start, '?', tab)||where_;

  paths text[] not null := '{}';
  p     text   not null := '';
  path  text[] not null := '{}';

  distance         int     not null := -1;
  match            text    not null := '';
  prev_match       text    not null := '';
  movies           text[]  not null := '{}';
  movie            text    not null := '';
  pad              int     not null := 0;
begin
  case terminal is null
    when true then
      execute all_terminals_stmt into paths;
    else
      execute one_terminal_stmt into paths using terminal;
  end case;

  foreach p in array paths loop
    path := p::text[];
    distance := cardinality(path) - 1;
    match := terminal(path);

    -- Rule off before each new match.
    case match = prev_match
      when false then
        t := rpad('-', 50, '-');                                 return next;
    end case;
    prev_match := match;

    pad := 0;
    t := rpad(' ', pad)||path[1];                                return next;
    <<step_loop>>
    for j in 2..cardinality(path) loop
      select e.movies
      into strict b.movies
      from edges e
      where e.node_1 = path[j - 1] and e.node_2 = path[j];

      pad := pad + indent;
      <<movies_loop>>
      foreach movie in array movies loop
        t := rpad(' ', pad)||movie::text;                        return next;
      end loop movies_loop;

      pad := pad + indent;
      t := rpad(' ', pad)||path[j];                              return next;
    end loop step_loop;
  end loop;
  t := rpad('-', 50, '-');                                       return next;
end b;
$body$;
```

## Computing Bacon Numbers for synthetic data and the real IMDb data

The section [Computing Bacon Numbers for a small set of synthetic actors and movies data](./synthetic-data/) demonstrates the approach using a small data set.

The section [Computing Bacon Numbers for real IMDb data](./imdb-data/) shows how to ingest the raw _"imdb.small.txt"_ file into the same representation that was used for the synthetic data. (The subsection [Download and ingest some IMDb data](./imdb-data/#download-and-ingest-some-imdb-data) explains how to download the IMDb subset that this case study uses.)

While a straightforward use of a `WITH` clause recursive substatement can be used to produce the solution for the small synthetic data set quickly, it fails to complete before crashing (see the section [Stress testing different find_paths() implementations on maximally connected graphs](../traversing-general-graphs/stress-test/)) when it's applied to the ingested *"imdb.small.txt"* data. The approach described in the [How to implement early path pruning](../traversing-general-graphs/undirected-cyclic-graph/#how-to-implement-early-path-pruning) section comes to the rescue.

