-- Copyright (c) YugaByte, Inc.

alter table kms_history add column active boolean default false;

-- Update the primary key to be agnostic to the kms configuration used
alter table kms_history drop constraint pk_kms_universe_key_history;
alter table kms_history add constraint pk_kms_universe_key_history primary key (target_uuid, type, key_ref);

-- Create a unique index to ensure at most one key ref is active per universe at a time
alter table kms_history add column target_uuid_active uuid as
      (case (active=true and type='UNIVERSE_KEY')
        when false then null
        else target_uuid
      end)
  unique;

update kms_history
set active = true
where (target_uuid, timestamp) in
  (select h.target_uuid, max(h.timestamp) as timestamp
    from kms_history h
    where h.type='UNIVERSE_KEY'
    group by h.target_uuid)
and type='UNIVERSE_KEY';
