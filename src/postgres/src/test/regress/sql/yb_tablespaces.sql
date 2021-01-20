-- Create Tablespace API tests
-- Create Tablespace should not work if neither LOCATION nor options
-- are specified
CREATE TABLESPACE x;
CREATE TABLESPACE x OWNER yugabyte;

-- Ill formed JSON
CREATE TABLESPACE x WITH (replica_placement='[{"cloud"}]');

-- One of the required keys missing
CREATE TABLESPACE x WITH (replica_placement='[{"cloud":"cloud1","region":"r1","zone":"z1"}]');

-- Invalid value for replication factor
CREATE TABLESPACE x WITH (replica_placement='[{"cloud":"cloud1","region":"r1","zone":"z1","min_number_of_replicas":"three"}]');

-- Positive cases
CREATE TABLESPACE x LOCATION '/data';
CREATE TABLESPACE y WITH (replica_placement='[{"cloud":"cloud1","region":"r1","zone":"z1","min_number_of_replicas":3},{"cloud":"cloud2","region":"r2", "zone":"z2", "min_number_of_replicas":3}]');

-- describe command
\db+
