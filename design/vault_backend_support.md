# YugabyteDB as Vault database backend (non-HA)
Following is the design overview for adding YugabyteDB as one of vault database backends. It won't support _High Availability_ for vault in the initial version; it may be added later.

## Prerequisites
To use YugabyteDB as backend for Vault, user will need to have following things:
- Installed a YugabyteDB Universe with YSQL enabled.
- Installed Go PostgreSQL client.

## Setup
Provide following parameters in vault configuration to use YugabyteDB as backend database:
- **host:** String. Required parameter. YugabyteDB host name
- **port:** Integer. Optional parameter. YSQL Port number. Optional param. It will be defaulted to 5433, if omitted.
- **user:** String. Required parameter. YSQL user name
- **password:** String. Required parameter. YSQL user password
- **dbname:** String. Required parameter. Database name. A database by this name must exist.
- **tablename:** String. Optional parameter. Name of the table to store data in. If table does not exist, it will be attempted to create. Default value being "vault_store".
- **max_idle_connections:** Integer. Optional parameter. Maximum connections in idle connection pool.
- **max_parallel:** Integer. Optional parameter. Maximum concurrent requests to YSQL. Default will be 128 (inline with other available backend supports for vault).


## Implementation
Implement following methods to support YugabyteDB as backend: <br/>
- **NewYugabyteDBBackend:** Read user configuration, create table if it doesn't exist & return an object of type YugabyteDBBackend
- **Get:** Fetch value for given key
- **Put:** Insert/Update given key-value
- **List:** Get all keys for given prefix
- **Delete:** Permanently delete a key-value entry.
