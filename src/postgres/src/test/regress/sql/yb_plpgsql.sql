--
-- Assure that some of the record tests taken from `plpgsql.sql` work not just
-- for temporary tables but also regular tables.  (Functions are taken from
-- `plpgsql.sql`.)
--
create temp table foo (f1 int, f2 int);
insert into foo values (1, 2), (3, 4), (5, 6), (5, 6), (7, 8), (9, 10);
select * from foo order by f1;

create table bar (f1 int, f2 int);
insert into bar values (1, 2), (3, 4), (5, 6), (5, 6), (7, 8), (9, 10);
select * from bar order by f1;

create or replace function stricttest1() returns void as $$
declare x record;
begin
  -- should work
  select * from foo where f1 = 3 into strict x;
  raise notice 'x.f1 = %, x.f2 = %', x.f1, x.f2;
end$$ language plpgsql;
create or replace function stricttest2() returns void as $$
declare x record;
begin
  -- should work
  select * from bar where f1 = 3 into strict x;
  raise notice 'x.f1 = %, x.f2 = %', x.f1, x.f2;
end$$ language plpgsql;
create or replace function stricttest3() returns void as $$
declare x record;
begin
  -- too many rows, no params
  select * from foo where f1 > 3 into strict x;
  raise notice 'x.f1 = %, x.f2 = %', x.f1, x.f2;
end$$ language plpgsql;
create or replace function stricttest4() returns void as $$
declare x record;
begin
  -- too many rows, no params
  select * from bar where f1 > 3 into strict x;
  raise notice 'x.f1 = %, x.f2 = %', x.f1, x.f2;
end$$ language plpgsql;
select stricttest1();

select stricttest2();

select stricttest3();
select stricttest4();
--
-- Cleanup
--
DROP TABLE foo;
DROP TABLE bar;
DROP FUNCTION stricttest1(), stricttest2(), stricttest3(), stricttest4();

-- Fail because collate and cursor are not supported.
create or replace function unsupported1() returns void as $$
declare a text collate "en_US";
begin
end$$ language plpgsql;

create table test(k int, v int);

create procedure intermediate_commit() as $$
begin
  insert into test values(1, 1);
  insert into test values(2, 2);
  commit;
  insert into test values(3, 3);
end$$ LANGUAGE plpgsql;

call intermediate_commit();

select * from test order by k;

do $$
begin
  insert into test values(4, 4);
  commit;
  insert into test values(5, 5);
  insert into test values(6, 6);
end $$;

select * from test order by k;

do $$
begin
  insert into test values(7, 7);
  -- commit inserting (7, 7) row and start new transaction automatically.
  commit;
  insert into test values(8, 8);
  rollback;
  -- only insertion of (8, 8) row is rolled back.
  -- new transaction is started automatically, next row will be inserted.
  insert into test values(9, 9);
end $$;

select * from test order by k;

create procedure p(a inout int)
  language plpgsql
as $body$
begin
  a := a + 1;
end;
$body$;

do $body$
declare
  a int := 10;
begin
  call p(a);
  raise info '%', a::text;
end;
$body$;
