-- Copyright (c) YugaByte, Inc.

alter table kms_history add column active boolean default false;

-- Update the primary key to be agnostic to the kms configuration used
alter table kms_history drop constraint pk_kms_universe_key_history;
alter table kms_history add constraint pk_kms_universe_key_history primary key (target_uuid, type, key_ref);

-- Create a unique index to ensure at most one key ref is active per universe at a time
create unique index single_active_universe_key on kms_history (target_uuid) where active=true and type='UNIVERSE_KEY';

update kms_history
set active = true
from (
    select h.target_uuid, max(h.timestamp) as timestamp
    from kms_history h
    where h.type='UNIVERSE_KEY'
    group by h.target_uuid
    ) as latest_history
where kms_history.timestamp=latest_history.timestamp
and kms_history.target_uuid=latest_history.target_uuid
and kms_history.type='UNIVERSE_KEY';
