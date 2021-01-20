-- Copyright (c) YugaByte, Inc.

alter table customer add column code varchar(5);

update customer set code = CAST(id as text) ;

alter table customer alter column code set not null;
