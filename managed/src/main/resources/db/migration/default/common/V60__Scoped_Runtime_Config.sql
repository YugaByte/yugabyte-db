-- Copyright (c) YugaByte, Inc.
CREATE table scoped_runtime_config (
  uuid                          UUID NOT NULL,
  -- At most one of the following foreign keys can be non null:
  customer_uuid                 UUID,
  universe_uuid                 UUID,
  provider_uuid                 UUID,
  CONSTRAINT pk_scoped_runtime_config PRIMARY KEY (uuid),
  CONSTRAINT fk_src_customer_uuid FOREIGN KEY (customer_uuid) REFERENCES customer(uuid) ON DELETE CASCADE ON UPDATE CASCADE,
  CONSTRAINT fk_src_universe_uuid FOREIGN KEY (universe_uuid) REFERENCES universe(universe_uuid) ON DELETE CASCADE ON UPDATE CASCADE,
  CONSTRAINT fk_src_provider_uuid FOREIGN KEY (provider_uuid) REFERENCES provider(uuid) ON DELETE CASCADE ON UPDATE CASCADE,
  -- CHECK to make sure only one of them is non null or the scope
  -- is global (uuid all zeroes)
  CHECK (
        (uuid = '00000000-0000-0000-0000-000000000000')::INTEGER +
        (customer_uuid IS NOT NULL)::INTEGER +
        (universe_uuid IS NOT NULL)::INTEGER +
        (provider_uuid IS NOT NULL)::INTEGER = 1)
);

-- create global scoped runtime config
INSERT INTO scoped_runtime_config (uuid) VALUES ('00000000-0000-0000-0000-000000000000');

CREATE TABLE runtime_config_entry (
  scope_uuid                UUID NOT NULL,
  path                      VARCHAR(127) CHECK (path <> ''),
  value                     VARCHAR(255),

  -- scope_uuid is part of primary key and a foreign key
  CONSTRAINT pk_rce_runtime_config_entry PRIMARY KEY (scope_uuid, path),
  CONSTRAINT fk_rce_scoped_runtime_config FOREIGN KEY (scope_uuid) REFERENCES scoped_runtime_config(uuid) ON DELETE CASCADE ON UPDATE CASCADE
);
