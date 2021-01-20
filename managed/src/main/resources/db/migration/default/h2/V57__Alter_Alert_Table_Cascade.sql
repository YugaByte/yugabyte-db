-- Copyright (c) YugaByte, Inc.
-- special case because of constraint name scope is per database in h2
ALTER TABLE alert DROP CONSTRAINT fk_alert_definition_uuid;
ALTER TABLE alert ADD CONSTRAINT fk_a_alert_definition_uuid FOREIGN KEY (definition_uuid) references alert_definition (uuid)  ON DELETE CASCADE ON UPDATE CASCADE;

ALTER TABLE alert_definition DROP CONSTRAINT fk_ad_customer_uuid;
ALTER TABLE alert_definition
  ADD CONSTRAINT fk_ad_customer_uuid FOREIGN KEY (customer_uuid) REFERENCES customer (uuid)
  ON DELETE CASCADE ON UPDATE CASCADE;

ALTER TABLE alert_definition DROP CONSTRAINT fk_ad_universe_uuid;
ALTER TABLE alert_definition
  ADD CONSTRAINT fk_ad_universe_uuid FOREIGN KEY (universe_uuid) REFERENCES universe (universe_uuid)
  ON DELETE CASCADE ON UPDATE CASCADE;
