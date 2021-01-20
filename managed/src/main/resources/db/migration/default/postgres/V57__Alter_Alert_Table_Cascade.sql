-- Copyright (c) YugaByte, Inc.
ALTER TABLE alert DROP CONSTRAINT fk_alert_definition_uuid;
ALTER TABLE alert ADD CONSTRAINT fk_alert_definition_uuid FOREIGN KEY (definition_uuid) references alert_definition (uuid)  ON DELETE CASCADE ON UPDATE CASCADE;

ALTER TABLE alert_definition DROP CONSTRAINT fk_customer_uuid;
ALTER TABLE alert_definition
  ADD CONSTRAINT fk_customer_uuid FOREIGN KEY (customer_uuid) REFERENCES customer (uuid)
  ON DELETE CASCADE ON UPDATE CASCADE;

ALTER TABLE alert_definition DROP CONSTRAINT fk_universe_uuid;
ALTER TABLE alert_definition
  ADD CONSTRAINT fk_universe_uuid FOREIGN KEY (universe_uuid) REFERENCES universe (universe_uuid)
  ON DELETE CASCADE ON UPDATE CASCADE;
