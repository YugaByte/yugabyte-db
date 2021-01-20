---
title: Enable encryption in transit (TLS)
headerTitle: Enable encryption in transit (TLS)
linkTitle: Enable encryption in transit (TLS)
description: Use Yugabyte Platform to enable encryption in transit (TLS) on a YugabyteDB universe and connect to clients.
menu:
  stable:
    parent: security
    identifier: enable-encryption-in-transit
    weight: 29
isTocNested: true
showAsideToc: true
---

Yugabyte Platform provides the ability to protect data in transit with:

* Server-server encryption for intra-node communication between YB-Master and YB-TServer nodes
* Client-server for communication between clients and nodes when using CLIs, tools, and APIs for YSQL and YCQL

{{< note title="Note" >}}

Client-server TLS encryption is not supported for YEDIS. Before you can enable client-to-server encryption, you first must enable server-to-server encryption.

{{< /note >}}

Yugabyte Platform provides the option to create a new self-signed certificate, use an existing self-signed certificate, or upload a third-party certificate from external providers, such as Venafi or DigiCert (only available for manual installations).

Currently, TLS encryption can be enabled or disabled during universe creation, but cannot be changed after a universe is created.  

## Self-signed platform generated certificates

Yugabyte Platform can create self-signed certificates for each universe and may be shared between universes within a single instance of Yugabyte Platform. The certificate name will have the following format: `yb-environment-universe_name` where the environment is the environment type (`dev`, `stg`, `demo`, or `prod`) that was used during the tenant registration (admin user creation), and the provided universe name. The platform will generate the root certificate, root private key, and any node-level certificates required (if node-to-node encryption is enabled), and provision those to the database nodes any time nodes are created or added to the cluster. Three files are copied to each node: the root certificate (`ca.cert`), the node certificate (`node.ip_address.crt`) and the node private key (`node.ip_address.key`). The platform will retain the root certificate and the root private key for all interactions with the cluster.

## Self-signed user-provided certificates

Instead of using platform-provided certificates, you can also bring your own self-signed certificates. The certificates must be uploaded to the platform using the procedure documented below. The certificates must be in CRT format and the private key in PEM format must be available to be uploaded. The certificates should contain IP addresses of the target nodes and/or the DNS names as the Subject Alternative Names (wildcards are acceptable). The platform will take the uploaded certificate and produce the node (leaf) certificates and copy the certificate chain, leaf certificate and private key to the nodes in the cluster. 

## Enable TLS using platform generated certificates

1. Create a new universe **Universes -> Create Universe**.
2. Configure the universe as desired.
3. Based on your needs, select **Enable Node-to-Node TLS** and **Enable Client-to-Node TLS**.
4. Choose an existing certificate from the **Root Certificate** drop-down list or leave as default “Create new certificate” if a new one is desired to be used.
5. Create the universe.

To view the certificate, select the drop-down list in the top-right corner of the Yugabyte Platform console and click **Certificates**.

## Enable TLS using a self-signed, user-provided certificate

1. In the top-right corner of the Yugabyte Platform console, click the drop-down list (with the profile icon) and select **Certificates**.
2. Click **Add Certificate**. The **Add Certificate** dialog appears.
3. Click **Upload Certificate File**, browse to the root certificate file (`<file-name>.crt`), and upload it.
4. Click **Upload Key File**, browse to the root certificate file (`<file-name>.key`), and upload it.
5. In **Name Certificate**, enter a meaningful name.
6. In **Expiration Date**, specify the expiration date of the certificate. The expiration date can be found by running the following command: `openssl x509 -in <root crt file path> -text -noout` and then looking for the **Validity Not After** date.
7. Click **Add**. The certificate is available.
8. Go to **Universes -> Create Universe**. The **Create Universe** dialog appears.
9. Configure the universe as desired.
10. Based on your needs, select **Enable Node-to-Node TLS** and **Enable Client-to-Node TLS**.
11. Choose an existing certificate from the **Root Certificate** drop-down list and then select the certificate that was uploaded.
12. Create the universe.

## Connect to clusters

### Connecting to the YSQL endpoint with TLS

If you enabled the Client-to-Node TLS option when you created your universe, then you must download client certificates to your client machine to connect to your database.

1. Go to the **Certificates** page, then to your universe’s certificate, and select the **Download YSQL Cert** option as shown below.

![Download YSQL Certificate](/images/yp/encryption-in-transit/download-ysql-cert.png)

This will download two files: `yugabytedb.crt` and `yugabytedb.key`.

2. For testing with a `ysqlsh` client, copy these files to `<home-dir>/.yugabytedb` directory and change the permissions to `0600`.

```sh
$ mkdir ~/.yugabytedb; cd ~/.yugabytedb
$ cp <DownloadDir>/yugabytedb.crt .
$ cp <DownloadDir>/yugabytedb.key .
$ chmod 600 yugabytedb.*
```

3. Run `ysqlsh` using tje `sslmode=require` option.

```sh
$ cd <yugabyte software install directory>
$ bin/ysqlsh -h 172.152.43.78 -p 5433 sslmode=require
ysqlsh (11.2-YB-2.3.3.0-b0)
SSL connection (protocol: TLSv1.2, cipher: ECDHE-RSA-AES256-GCM-SHA384, bits: 256, compression: off)
Type "help" for help.

yugabyte=#
```

To use TLS from a different client, see the client-specific documentation. For example, if you’re using a Postgres JDBC driver to connect to YugabyteDB, see [Using SSL – Configuring the Client](https://jdbc.postgresql.org/documentation/head/ssl-client.html) for more details.

### Connecting to the YCQL endpoint with TLS

If you enabled the Client-to-Node TLS option when you created your universe, then you must download the Root CA certificate to your client machine to connect to the database.

1. Go to **Certificates** page, navigate to your universe’s certificate, and select **Download Root Cert** option as shown below.

![Download Root Cert](/images/yp/encryption-in-transit/download-root-cert.png)

This will download a file called `root.crt`.

2. Set `SSL_CERTFILE` environment variable to where you saved the downloaded root certificate.

3. Run `ycqlsh` using `-ssl` option.

```sh
$ cp <DownloadDir>/root.crt ~/.yugabytedb/root.crt
$ export SSL_CERTFILE=~/.yugabytedb/root.crt
$ bin/ycqlsh 172.152.43.78 --ssl
Connected to local cluster at 172.152.43.78:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
ycqlsh>
```

To use TLS from a different client, see the client-specific documentation. For example, if you’re using a Cassandra driver to connect to YugabyteDB, see [Security – SSL](https://docs.datastax.com/en/developer/python-driver/3.19/security/#ssl) for more details.
