create table health_check (
  universe_uuid                 uuid not null,
  check_time                    timestamp not null,
  customer_id                   bigint,
  details_json                  TEXT not null,
  constraint pk_health_check primary key (universe_uuid, check_time)
);

