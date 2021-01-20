-- Copyright (c) YugaByte, Inc.

UPDATE universe SET universe_details_json = replace(universe_details_json, 'ToBeDecommissioned', 'ToBeRemoved');
UPDATE universe SET universe_details_json = replace(universe_details_json, 'BeingDecommissioned', 'BeingRemoved');
UPDATE universe SET universe_details_json = replace(universe_details_json, 'Destroyed', 'Removed');
UPDATE universe SET universe_details_json = replace(universe_details_json, 'Running', 'Live');
