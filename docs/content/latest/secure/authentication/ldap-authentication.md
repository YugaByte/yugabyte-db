---
title: LDAP Authentication
headerTitle: LDAP Authentication
linkTitle: LDAP Authentication
description: Configuring YugabyteDB to use an external LDAP authentication service.
menu:
  latest:
    identifier: ldap-authentication
    parent: authentication
    weight: 732
isTocNested: false
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/secure/authentication/ysql-authentication" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
</ul>

The ldap authentication method is similar to the password method, except that it uses LDAP to verify the password. Therefore, before LDAP can be used for authentication, the user must already exist in the database (and have appropriate permissions). 

LDAP Authentication can be enabled in the YugabyteDB cluster by setting the LDAP configuration with <code>[--ysql_hba_conf](https://docs.yugabyte.com/latest/reference/configuration/yb-tserver/#ysql-hba-conf)</code> flag. YugabyteDB supports two modes for LDAP authentication: 

* <strong>simple-bind</strong> mode
*  <strong>search+bind</strong> mode

These are described below.

## Simple Bind Mode

In **simple-bind** mode, YB-TServer will bind to the Distinguished Name (“DN”) constructed with “prefix username suffix” format. Here is an example for Simple bind mode 


```
--ysql_hba_conf_csv='host all yugabyte 127.0.0.1/0 password,"host   all         all      0.0.0.0/0  ldap ldapserver=ldap.yugabyte.com ldapprefix=""uid="" ldapsuffix="", ou=DBAs, dc=example, dc=com"" ldapport=389"'
```


### Configurations

The configurations supported for simple bind mode.


<table>
  <tr>
   <td><strong>ldapserver</strong>
   </td>
   <td>Names or IP addresses of LDAP servers to connect to. Multiple servers may be specified, separated by spaces.
   </td>
  </tr>
  <tr>
   <td><strong>ldapport</strong> 
   </td>
   <td>Port number on LDAP server to connect to. If no port is specified, LDAP default port 389 will be used.
   </td>
  </tr>
  <tr>
   <td><strong>ldapscheme</strong>
   </td>
   <td>Set to ldaps to use LDAPS. This is a non-standard way of using LDAP over SSL, supported by some LDAP server implementations. See also the ldaptls option for an alternative.
   </td>
  </tr>
  <tr>
   <td><strong>ldaptls</strong>
   </td>
   <td>Set to 1 to make the connection between PostgreSQL and the LDAP server use TLS encryption. 
   </td>
  </tr>
  <tr>
   <td><strong>ldapprefix</strong>
   </td>
   <td>String used for prepending the user name when forming the DN for binding to the LDAP server.
   </td>
  </tr>
  <tr>
   <td><strong>ldapsuffix</strong>
   </td>
   <td>String used for appending the user name when forming the DN for binding to the LDAP Server.
   </td>
  </tr>
</table>


## Search + Bind Mode

In `Search + Bind` mode, YB-Tserver will bind to the LDAP directory with a fixed username and password, specified with `ldapbinddn` and `ldapbindpasswd`, and performs a search for the user trying to log in to the database. This mode is commonly used by LDAP authentication schemes in other softwares.

For Searching the LDAP directory if no fixed username and password is configured at YB-TServer, an anonymous bind will be attempted to the directory. The search will be performed over the subtree at `ldapbasedn`, and will try to do an exact match of the attribute specified in `ldapsearchattribute`. Once the user has been found in this search, the server disconnects and re-binds to the directory as this user, using the password specified by the client, to verify that the login is correct.

Here is an example for search + bind mode


```
--ysql_hba_conf_csv='host all yugabyte 127.0.0.1/0 password,"host   all         all      0.0.0.0/0  ldap ldapserver=ldap.yugabyte.com ldapbasedn=""dc=yugabyte, dc=com"" ldapsearchattribute=uid"'
```


### Configurations

The configurations supported for search + bind mode


<table>
  <tr>
   <td><strong>ldapserver</strong>
   </td>
   <td>Names or IP addresses of LDAP servers to connect to. Multiple servers may be specified, separated by spaces.
   </td>
  </tr>
  <tr>
   <td><strong>ldapport</strong> 
   </td>
   <td>Port number on LDAP server to connect to. If no port is specified, LDAP default port 389 will be used.
   </td>
  </tr>
  <tr>
   <td><strong>ldapscheme</strong>
   </td>
   <td>Set to ldaps to use LDAPS. This is a non-standard way of using LDAP over SSL, supported by some LDAP server implementations. See also the ldaptls option for an alternative.
   </td>
  </tr>
  <tr>
   <td><strong>ldaptls</strong>
   </td>
   <td>Set to 1 to make the connection between PostgreSQL and the LDAP server use TLS encryption. 
   </td>
  </tr>
  <tr>
   <td><strong>ldapbasedn</strong>
   </td>
   <td>Specifies the base directory to begin the user name search 
   </td>
  </tr>
  <tr>
   <td><strong>ldapbinddn</strong>
   </td>
   <td>Specifies the username to perform the initial search when doing search + bind authentication
   </td>
  </tr>
  <tr>
   <td><strong>ldapbindpasswd</strong>
   </td>
   <td>Password for username being used to perform the initial search when doing search + bind authentication
   </td>
  </tr>
  <tr>
   <td><strong>ldapsearchattribute</strong>
   </td>
   <td>Attribute to match against the username in the search when doing search + bind authentication. If no attribute is specified, the <strong>uid </strong>attribute is used.
   </td>
  </tr>
  <tr>
   <td><strong>ldapsearchfilter</strong>
   </td>
   <td>The search filter to use when doing search + bind authentication.
   </td>
  </tr>
  <tr>
   <td><strong>ldapurl</strong>
   </td>
   <td>An RFC 4516 LDAP URL. This is an alternative way to write LDAP options in a more compact and standard form.
   </td>
  </tr>
</table>


## Example

To use LDAP password authentication on a new YugabyteDB cluster, follow these steps



1. Use  `--ysql_hba_conf_csv` config flag to enable LDAP authentication on YB-TServer. Use the below configuration to start a YugabyteDB cluster.

    ```
    --ysql_hba_conf_csv='host all yugabyte 127.0.0.1/0 password,"host   all         all      0.0.0.0/0  ldap ldapserver=ldap.forumsys.com ldapprefix=""uid="" ldapsuffix="", dc=example, dc=com"" ldapport=389"'
    ```

    {{< note title="Note" >}}
    In the above sample configuration, we are using an [online LDAP test server](https://www.forumsys.com/tutorials/integration-how-to/ldap/online-ldap-test-server/) for setting up the LDAP authentication with YugabyteDB.
    {{< /note >}}


    For Convenience we use two host based authentication (HBA) rules. 

    *   The first HBA rule` host all yugabyte 127.0.0.1/0 password` allows access from the localhost (127.0.0.1) to the admin user (yugabyte) with password authentication. This allows the administrator to log in with the yugabyte user for setting up the roles (and permissions) for the LDAP users.
    *   The second HBA rule configures LDAP authentication for all other user/host pairs. We use simple bind with a uid-based username (ldapprefix) and a suffix defining the domain component (dc).



2. Start the YugabyteDB cluster.

3. Open the YSQL shell (ysqlsh), specifying the `yugabyte` user and prompting for the password.

    ```
    $ ./ysqlsh -U yugabyte -W
    ```

    When prompted for the password, enter the yugabyte password (default is `yugabyte`). You should be able to login and see a response like below.


    ```
    ysqlsh (11.2-YB-2.3.3.0-b0)
    Type "help" for help.

    yugabyte=#
    ```


4. To display the current values in the` ysql_hba.conf` file, run the following `SHOW` statement to get the file location:

    ```
    yugabyte=# SHOW hba_file;

                         hba_file
    -------------------------------------------------------
     /Users/yugabyte/yugabyte-data/node-1/disk-1/pg_data/ysql_hba.conf
    (1 row)
    ```



    and then view the file. The `ysql_hba.conf` file should have the following configuration


    ```
    # This is an autogenerated file, do not edit manually!
    host all yugabyte 127.0.0.1/0 trust
    host   all         all      0.0.0.0/0  ldap ldapserver=ldap.forumsys.com ldapprefix="uid=" ldapsuffix=", dc=example, dc=com" ldapport=389
    ```


5. Configure database role(s) for the LDAP user(s)


```
  We are creating a ROLE for username riemann supported by the test LDAP server. 

  yugabyte=# CREATE ROLE riemann WITH LOGIN;
  yugabyte=# GRANT ALL ON DATABASE yugabyte T0 riemann;

```



6. Connect using LDAP authentication

    Connect ysqlsh using `riemann` LDAP user and password specified in the [Online LDAP Test Server](https://www.forumsys.com/tutorials/integration-how-to/ldap/online-ldap-test-server/) page. 


    ```
    $ ./ysqlsh -U riemann -W
    ```



    You can confirm the current user by running the following command


    ```
    yugabyte=# SELECT current_user;
     current_user
    --------------
     riemann
    (1 row)
```