-- Testing pgcrypto.
create extension pgcrypto;

select digest('xyz', 'sha1');

-- Using fixed salt to make test repeatable.
select crypt('new password', '$1$7kF93Vc4');

-- Using count to make test repeatable.
select count(gen_random_uuid());

-- Testing fuzzystrmatch.
create extension fuzzystrmatch;

select levenshtein('YugaByte', 'yugabyte');

select metaphone('yugabyte', 8);

-- Clean up.
drop extension pgcrypto;

-- Expect failure since function should be removed.
select digest('xyz', 'sha1');

drop extension fuzzystrmatch;

-- Expect failure since function should be removed.
select levenshtein('YugaByte', 'yugabyte');

-- Testing pg_stat_statements;
select pg_stat_statements_reset();
select userid,dbid,queryid,query,calls,rows,shared_blks_hit,shared_blks_read,shared_blks_dirtied,shared_blks_written,local_blks_hit,local_blks_read,local_blks_dirtied,local_blks_written,temp_blks_read,temp_blks_written,blk_read_time from pg_stat_statements;

create table test(a int, b float);
insert into test(a,b) values (5,10);
select userid,dbid,queryid,query,calls,rows,shared_blks_hit,shared_blks_read,shared_blks_dirtied,shared_blks_written,local_blks_hit,local_blks_read,local_blks_dirtied,local_blks_written,temp_blks_read,temp_blks_written,blk_read_time from pg_stat_statements;
insert into test(a,b) values (15,20);
select userid,dbid,queryid,query,calls,rows,shared_blks_hit,shared_blks_read,shared_blks_dirtied,shared_blks_written,local_blks_hit,local_blks_read,local_blks_dirtied,local_blks_written,temp_blks_read,temp_blks_written,blk_read_time from pg_stat_statements;
drop table test;

-- Testing uuid-ossp
create extension "uuid-ossp";

select uuid_generate_v5('00000000-0000-0000-0000-000000000000', 'yugabyte');

-- generated values are random, so to ensure deterministic output ignore the actual values.
select uuid_generate_v1() != uuid_generate_v4();
