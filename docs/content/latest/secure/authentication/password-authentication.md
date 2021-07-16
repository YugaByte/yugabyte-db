---
title: Password Authentication
headerTitle: Password Authentication
linkTitle: Password Authentication
description: Use SCRAM-SHA-256 password authentication to strengthen your YugyabyteDB security.
menu:
  latest:
    identifier: password-authentication
    parent: authentication
    weight: 731
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/authentication/password-authentication" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
</ul>

By default, password authentication is disabled, allowing users and clients to connect to and interact with YugabyteDB with minimal effort. For production clusters, password authentication is important for maximizing the security. The password authentication methods work similarly, but differ in how user passwords are stored on the server and how the password provided by the client is sent across the connection.

## Password authentication methods

The following password authentication methods are supported by YugabyteDB.

### MD5

The MD5 method (`md5`) prevents password sniffing and avoids storing passwords on the server in plain text, but provides no protection if an attacker obtains password hashes from the server or from clients (by sniffing, man-in-the-middle, or by brute force).  This method is the default password encryption for YugabyteDB clusters.

The MD5 hash algorithm is not considered secure against determined attacks. Some of the security risks include:

- If someone has access to a valid username/password combination, or their MD5-styled hash, they can log into any cluster where that user exists with the same username and password.
- The "shared secret" is effectively shared over the wire every time the MD5 authentication method is used.

### SCRAM-SHA-256

The SCRAM-SHA-256 method (`scram-sh-256`) performs SCRAM-SHA-256 authentication, as described in [RFC 7677](https://tools.ietf.org/html/rfc7677). This challenge-response scheme prevents password sniffing on untrusted connections and supports storing passwords on YugabyteDB clusters in the most secure cryptographically hashed form available. The SCRAM-SHA-256 method implemented here is explained in further detail in [SASL Authentication (PostgreSQL documentation)](https://www.postgresql.org/docs/11/sasl-authentication.html). This is the most secure password authentication available and is supported by most of the [client drivers for the YSQL API](../../../reference/drivers/ysql-client-drivers).

- Allows for two parties to verify they both know a secret without exchanging the secret.
- SCRAM-SHA-256 encryption uses the [SASL authentication mechanism flow](https://www.postgresql.org/docs/11/sasl-authentication.html) to limit security risks from brute force attacks and sniffing.  

{{< note title="Note" >}}

For additional security, SCRAM-SHA-256 password encryption can also be used with [encryption in transit (TLS encryption)](../../../secure/tls-encryption).

{{< /note >}}

## YugabyteDB database passwords

YugabyteDB database passwords are separate from operating system passwords. The password for each database user is stored in the `pg_authid` system catalog.

Database passwords can be managed using the following:

- YSQL API: [CREATE ROLE](../../../api/ysql/the-sql-language/statements/dcl_create_role) and [ALTER ROLE](../../../api/ysql/the-sql-language/statements/dcl_alter_role)
- `ysqlsh` metacommand: [`\password`](../../../admin/ysqlsh/#password-username)

## Enable SCRAM-SHA-256 authentication

To configure a YugabyteDB cluster to use SCRAM-SHA-256 authentication for databases, follow these steps.

1. Change the password encryption to use SCRAM-SHA-256.

To change the default MD5 password encryption to use SCRAM-SHA-256, add the YB-TServer [`--ysql_pg_conf`](../../../reference/configuration/yb-tserver/#ysql-pg-conf) flag and set the value to `scram-sha-256`:

```sh
--ysql_pg_conf="password_encryption=scram-sha-256"
```

or in the `yb-tserver.conf`, add the following line:

```
--ysql_pg_conf=password_encryption=scram-sha-256
```

2. Specify the rules for host-based authentication.

To specify rules for the use of the `scram-sha-256` authentication method, add the YB-TServer [`--ysql_hba_conf_csv`](../../../reference/configuration/yb-tserver/#ysql-hba-conf-csv) 
flag and specify rules that satisfy your security requirements.

In the following example, the `--ysql_hba_conf_csv` flag modifies the default rules that use `trust` to use 
SCRAM-SHA-256 authentication, changing the default values of `trust` to use `scram-sha-256`:

```
--ysql_hba_conf_csv='host all all 0.0.0.0/0 scram-sha-256,host all all ::0/0 scram-sha-256'
```

or in the `yb-tserver.conf`, add the following line:

```
--ysql_hba_conf_csv=host all all 0.0.0.0/0 scram-sha-256,host all all ::0/0 scram-sha-256
```

For details on using the [--ysql_hba_conf_csv](../../../reference/configuration/yb-tserver/#ysql-hba-conf-csv) flag to 
specify rules that satisfy your security requirements, see [Fine-grained authentication](../host-based-authentication).

## Create a cluster that uses SCRAM-SHA-256 password authentication

To use SCRAM-SHA-256 password authentication on a new YugabyteDB cluster, follow this procedure:

1. In the YB-TServer configuration file (flagfile), add the following two lines: 

```sh
--ysql_pg_conf=password_encryption=scram-sha-256
--ysql_hba_conf_csv=host all all 0.0.0.0/0 md5,host all all ::0/0 md5,host all all 0.0.0.0/0 scram-sha-256,host all all ::0/0 scram-sha-256
```

- The first line starts your YugabyteDB cluster with password encryption set to encrypt all *new* passwords using SCRAM-SHA-256. 
- The `ysql_hba_conf_csv` flag above specifies rules that allow both MD5 and SCRAM-SHA-256 *existing* passwords to be used to connect to databases. 

2. Start the YugabyteDB cluster.

3. Open the YSQL shell (`ysqlsh`), specifying the `yugabyte` user and prompting for the password.

```sh
$ ./ysqlsh -U yugabyte -W
```

When prompted for the password, enter the `yugabyte` password (default is `yugabyte`). You should be able to log in and see a response like this:

```
ysqlsh (11.2-YB-2.3.3.0-b0)
Type "help" for help.

yugabyte=#
```

4. Change the password for `yugabyte` to a SCRAM-SHA-256 password.

You can use either the ALTER ROLE statement or the `ysqlsh` `\password\` metacommand to change the password. 
The new password is encrypted using the SCRAM-SHA-256 hashing algorithm. In the following example, the `\password` metacommand is used to change the password.

```sh
yugabyte=# \password
```

You will be prompted twice for the new password and then returned to the YSQL shell prompt.

```
Enter new password:
Enter it again:
yugabyte=#
```

5. Stop the YugabyteDB cluster.

6. Remove the MD5 rules from the `--ysql_hba_conf_csv` flag.

In the flagfile, the updated flag should appear like this:

```sh
--ysql_hba_conf_csv=host all all 0.0.0.0/0 scram-sha-256,host all all ::0/0 scram-sha-256
```

7. Restart the YugabyteDB cluster.

8. Open the YSQL shell and log in, specifying the `yugabyte` user and password prompt.

```sh
$ ./ysqlsh -U yugabyte -W
```

When prompted, the changed `yugabyte` user password should get you access. Any new users or roles that you create will be encrypted using SCRAM-SHA-256. 
Access to the host and databases is determined by the rules you specify in the YB-TServer `--ysql_hba_conf_csv` configuration flag.

## Migrate existing MD5 passwords to SCRAM-SHA-256

When you [enable SCRAM-SHA-256 authentication](#enable-scram-sha-256-authentication) on an existing YugabyteDB cluster that has users and roles, with their MD5 passwords), you need to be aware that:

- All new, or changed, passwords will be encrypted using the SCRAM-SHA-256 hashing algorithm.
- All existing passwords were encrypted using the MD5 hashing algorithm.

Because all existing passwords must be changed, you can manage the migration of these user and role passwords from MD5 to SCRAM-SHA-256 
by maintaining rules in the `--ysql_hba_conf_csv` setting to allow both MD5 passwords and SCRAM-SHA-256 passwords to work until 
all passwords have been migrated to SCRAM-SHA-256. For an example, see [Create a cluster that uses SCRAM-SHA-256 password authentication](#Create-a-cluster-that-uses-scram-sha-256-password-authentication) above. 
If you follow a similar approach for an existing cluster, you can enhance your cluster security, track and migrate passwords, and then remove the much weaker MD5 rules after all passwords have been updated.

## Resetting user password

In PostgreSQL if the administrator password is lost or changed to an unknown value the `pg_hba.conf` can be modified to allow 
administrator access without a password. In PostgreSQL this is a static file that is used to control client authentication. 
To reset the password for the `postgres` user, parameters are modified in this configuration file, the database is restarted, and then 
the `postgres` user can login as `postgres` without a password, and reset the password.

The same is also true for YugabyteDB, although the implementation is slightly different. YugabyteDB has a `ysql_hba.conf` file similar to Postgres. 
However, unlike PostgreSQL, the contents of the file are dynamically generated leveraging a flag called [`--ysql_hba_conf_csv`](../../../reference/configuration/yb-tserver/#ysql-hba-conf-csv) 
at yb-tserver startup. The following steps will outline how the use of this flag can allow administrative access for the `yugabyte` user if the 
password is ever lost or changes to an unknown value.

The `ysql_hba.conf` file can be modified to allow administrator access without a password by changing the `--ysql_hba_conf_csv` configuration flag in the yb-tserver which 
we'll connect to reset the password. This is done by setting the flag as below and restarting the yb-tserver:

```
--ysql_hba_conf_csv=host all yugabyte 0.0.0.0/0 trust,host all all 0.0.0.0/0 md5,host all yugabyte ::0/0 trust,host all all ::0/0 md5
```

After restarting the yb-tserver, password authentication will be enforced for all users except `yugabyte` user. Now we 
can connect without a password:

```sh
$ ./bin/ysqlsh
```
And update the password of the user to new desired password:

```postgresql
yugabyte=# ALTER ROLE yugabyte WITH PASSWORD 'new-password';
```

Rollback the configuration and restart the yb-tserver to enable password authentication for `yugabyte` user again.
