
---
title: Column-Level Encryption
headerTitle: Column-Level Encryption
linkTitle: Column-Level Encryption
description: Using column-level encryption in a YugabyteDB cluster.
headcontent: Enable encryption at rest with a user-generated key
image: /images/section_icons/secure/prepare-nodes.png
menu:
  latest:
    identifier: column-level-encryption
    parent: secure
    weight: 745
isTocNested: true
showAsideToc: true
---



<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/secure/authorization/rbac-model" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

</ul>

YugabyteDB provides column level encryption to restrict access to sensitive data like Address, Credit card details. YugabyteDB uses PostgresQL `pgcrypto` extension to enable column level encryption, `PGP_SYM_ENCRYPT ` and ` PGP_SYM_DECRYPT `functions of pgcrypto extension is used to encrypt and decrypt column data.


## Symmetric Encryption

Steps for enabling Symmetric column encryption in YugabyteDB


### Step 1. Enable `pgcrypto` extension

Open the YSQL shell (ysqlsh), specifying the `yugabyte` user and prompting for the password.

```
$ ./ysqlsh -U yugabyte -W
```



When prompted for the password, enter the yugabyte password. You should be able to login and see a response like below.


```
ysqlsh (11.2-YB-2.5.0.0-b0)
Type "help" for help.

yugabyte=#
```

Enable `pgcrypto` extension on the YugabyteDB cluster `yugabyte=> \c yugabyte yugabyte;`


```
You are now connected to database "yugabyte" as user "yugabyte".

yugabyte=# CREATE EXTENSION IF NOT EXISTS pgcrypto;
CREATE EXTENSION

```

### Step 2. Insert using `PGP_SYM_ENCRYPT`

Create `employees` table and insert data into the table using `PGP_SYM_ENCRYPT` function for columns that need to be encrypted.

```
yugabyte=# create table employees ( empno int, ename text, address text, salary int, account_number text );
CREATE TABLE
```



In this example, account numbers of `employees` table will be encrypted using` PGP_SYM_ENCRYPT` function. 


```
yugabyte=# insert into employees values (1, 'joe', '56 grove st',  20000, PGP_SYM_ENCRYPT('AC-22001', 'AES_KEY'));
INSERT 0 1
yugabyte=# insert into employees values (2, 'mike', '129 81 st',  80000, PGP_SYM_ENCRYPT('AC-48901', 'AES_KEY'));
INSERT 0 1
yugabyte=# insert into employees values (3, 'julia', '1 finite loop',  40000, PGP_SYM_ENCRYPT('AC-77051', 'AES_KEY'));
INSERT 0 1
```


### Step 3. Verify column encryption 

Review the encrypted `account_number` data, as shown below


```
yugabyte=# select ename, account_number from employees limit 1;
 ename |               account_number
-------+-------------------------------------------------
 joe   | \xc30d04070302ee4c6d5f6656ace96ed23901f56c717d4e
 162b6639429f516b5103acebc4bc91ec15df06c30e29e6841f4a5386
 e7698bfebb49a8660f9ae4b3f34fede3f28c9c7bb245bd
(1 rows)
```



### Step 4. Query using `PGP_SYM_DECRYPT`

Decrypt the account numbers using `PGP_SYM_DECRYPT` function as shown here. In order to retrieve the encrypted column data, use `PGP_SYM_DECRYPT` function to decrypt the data. The Decryption function needs to be used in both SELECT and WHERE clause depending on the query. 


To allow the decryption, the field name is also casted to the binary data type with the syntax: ` account_number:bytea`.


```
yugabyte=# select PGP_SYM_DECRYPT(account_number::bytea, 'AES_KEY') as AccountNumber
           from employees;
 accountnumber
---------------
 AC-22001
 AC-48901
 AC-77051
(3 rows)
```


## Asymmetric Encryption

Asymmetric Encryption, also known as Public-Key cryptography can be used with YugabyteDB for enabling column level encryption. YugabyteDB can be configured with generated public/private keys or corporate gpg keys for encrypting column data.


The example below walks through configuring YugabyteDB cluster with a new set of keys (public and private)


### Step 1. Generate RSA key pair

* Start by generating new public and private RSA key pair using `gpg` key generator

    ```
    gpg --gen-key

    $ gpg --gen-key
    gpg (GnuPG) 2.0.22; Copyright (C) 2013 Free Software Foundation, Inc.
    This is free software: you are free to change and redistribute it.
    There is NO WARRANTY, to the extent permitted by law.
    ```

* After going through configuration prompts, RSA key gets generated - 


    ```
    public and secret key created and signed.

    pub   rsa2048 2020-11-09 [SC] [expires: 2022-11-09]
          043E14210E7628F93383D78EA2969FF91871CE06
    uid                      ybadmin <ybadmin@yugabyte.com>
    sub   rsa2048 2020-11-09 [E] [expires: 2022-11-09]
    ```


* Next, export public and private key of newly generated RSA key


  * Private Key

    ```
      $ gpg --export-secret-keys \
            --armor 043E14210E7628F93383D78EA2969FF91871CE06 > ./private_key.txt
    ```

  * Public Key

    ```
    $ gpg --export --armor 043E14210E7628F93383D78EA2969FF91871CE06 > ./public_key.txt

    ```


### Step 2. Enable `pgcrypto` extension

* Open the YSQL shell (ysqlsh), specifying the `yugabyte` user and prompting for the password.

    ```
    $ ./ysqlsh -U yugabyte -W
    ```

    When prompted for the password, enter the yugabyte password. You should be able to login and see a response like below.

    ```
    ysqlsh (11.2-YB-2.5.0.0-b0)
    Type "help" for help.

    yugabyte=#
    ```


* Enable `pgcrypto` extension on the YugabyteDB cluster

    ```
    You are now connected to database "yugabyte" as user "yugabyte".

    yugabyte=# CREATE EXTENSION IF NOT EXISTS pgcrypto;
    CREATE EXTENSION
    ```


### Step 3. Insert using `pgp_pub_encrypt`

Create `employees` table and insert data into the table using generated Public key for encrypting column data


```
yugabyte=# create table employees ( empno int, ename text, address text, salary int, account_number text );
CREATE TABLE
```

In this example, account numbers of `employees` table will be encrypted using `pgp_pub_encrypt` function and the generated Public key,

```
yugabyte=# insert into employees values (1, 'joe', '56 grove st',  20000, PGP_PUB_ENCRYPT('AC-22001', dearmor('-----BEGIN PGP PUBLIC KEY BLOCK----- XXXX  -----END PGP PUBLIC KEY BLOCK-----')));
INSERT 0 1
yugabyte=# insert into employees values (2, 'mike', '129 81 st',  80000, PGP_PUB_ENCRYPT('AC-48901', dearmor('-----BEGIN PGP PUBLIC KEY BLOCK----- XXXX  -----END PGP PUBLIC KEY BLOCK-----')));
INSERT 0 1
yugabyte=# insert into employees values (3, 'julia', '1 finite loop',  40000, PGP_PUB_ENCRYPT('AC-77051', dearmor('-----BEGIN PGP PUBLIC KEY BLOCK----- XXXX  -----END PGP PUBLIC KEY BLOCK-----')));
INSERT 0 1
```

### Step 4. Verify column encryption

Verify that the data for the column `account_number` is encrypted as shown below.


```
yugabyte=# select ename, account_number from employees limit 1;
 ename |                   account_number
-------+------------------------------------------------------------
 julia | \xc1c04c039bd2f02876cc14ae0107ff44e68e5a4bb35784b426f4aeb46
 70976127d64e731cf8f70343b100ea0ed60b3de191fa19e245c4ce9b0289e44b53b
 7d3c42b8187487b3b0bb8ebed518a248ca3c1d663174d1c9d6769f7840ddbd8508d
 d4b91dcf77183779ff15b003431a7d05a1aef4b09313b602bcc2491cc2e417d5c39
 269230e032252547ce1fd51f27be0cc43c5fd75f35b21e0a72e8e
(1 row)
```

### Step 5. Query using `pgp_pub_decrypt`

Use `pgp_pub_decrypt` and private key for decrypting column data. In order to retrieve the encrypted column data, use `pgp_pub_decrypt` function to decrypt the data and wrap the pgp private key with dearmor function to convert the private key into PGP ASCII-armor format. The Decryption function needs to be used in both SELECT and WHERE clause depending on the query. 

To allow the decryption, the field name is also casted to the binary data type with the syntax: ` account_number:bytea`.

```
yugabyte=# select PGP_PUB_DECRYPT(account_number::bytea,dearmor('-----BEGIN PGP PRIVATE KEY BLOCK----- XXXX  -----END PGP PRIVATE KEY BLOCK-----'),'PRIVATE-KEY-PASSWORD') as AccountNumber from employees;
 accountnumber
---------------
 AC-22001
 AC-48901
 AC-77051
(3 rows)
```

