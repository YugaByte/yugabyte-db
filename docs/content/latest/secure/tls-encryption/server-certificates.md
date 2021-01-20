---
title: Create server certificates
headerTitle: Create server certificates
linkTitle: Create server certificates
description: Generate server certificates and prepare YugabyteDB nodes for server-to-server encryption.
headcontent: Generate server certificates and prepare YugabyteDB nodes for server-to-server encryption.
image: /images/section_icons/secure/prepare-nodes.png
aliases:
  - /secure/tls-encryption/prepare-nodes
  - /latest/secure/tls-encryption/prepare-nodes
menu:
  latest:
    identifier: prepare-nodes
    parent: tls-encryption
    weight: 10
isTocNested: true
showAsideToc: true
---

Before you can enable server-to-server and client-to-server encryptions using Transport Security Layer (TLS), you need to prepare each node in a YugabyteDB cluster.

## Create the server certificates

### Create a secure data directory

To generate and store the secure information, such as the root certificate, create a directory, `secure-data`, in your root directory. After completing the preparation, you will copy this data inti a secure location and then delete this directory.

```sh
$ mkdir secure-data
```

### Create temporary node directories

Now create one directory for each node and put all the required data in that directory. This files added to each directory will be copied into the `tls-cert` directory on each node.

```sh
$ mkdir 127.0.0.1/ 127.0.0.2/ 127.0.0.3/
```

You should now have three directories, named `127.0.0.1`, `127.0.0.2`, and `127.0.0.3`, representing the three nodes of your YugabyteDB cluster.

### Create the root configuration file

Create the file `ca.conf` in the `secure-data` directory with the OpenSSL CA configuration.

```sh
$ cat > secure-data/ca.conf
```

Paste the following example root configuration into the file.

```sh
####################################
# Example CA root configuration file
####################################

[ ca ]
default_ca = my_ca

[ my_ca ]
# Validity of the signed certificate in days.
default_days = 3650

# Text file with next hex serial number to use.
serial = ./serial.txt

# Text database file to use, initially empty.
database = ./index.txt

# Message digest algorithm. Do not use MD5.
default_md = sha256

# a section with a set of variables corresponding to DN fields
policy = my_policy

[ my_policy ]

# Policy for nodes and users. If the value is "match" then 
# field value must match the same field in the CA certificate.
# If the value is "supplied" then it must be present. Optional
# means it may be present.
organizationName = supplied
commonName = supplied

[req]
prompt=no
distinguished_name = my_distinguished_name
x509_extensions = my_extensions

[ my_distinguished_name ]
organizationName = Yugabyte
commonName = CA for YugabyteDB

[ my_extensions ]
keyUsage = critical,digitalSignature,nonRepudiation,keyEncipherment,keyCertSign
basicConstraints = critical,CA:true,pathlen:1
```

To save and close the file, enter `Ctl+D`.

### Add required files

Create the index file (`index.txt`) and database file (`serial.txt`) by running the following commands.

```sh
$ touch index.txt
$ echo '01' > serial.txt
```

### Generate the root key (ca.key)

To generate the root private key file (`ca.key`) in the `secure-data` directory, run the following `openssl genrsa` command.

```sh
$ openssl genrsa -out secure-data/ca.key
```

You should see output like this:

```
Generating RSA private key, 2048 bit long modulus (2 primes)
......................+++++
.................+++++
e is 65537 (0x010001)
```

Change the access permissions of the generated private key to allow read-only privileges by running the `chmod` command.

```sh
$ chmod 400 secure-data/ca.key
```

## Generate the root certificate file

Next, generate the root certificate file (`ca.crt`) by running the following `openssl req` command.

```sh
$ openssl req -new                      \
            -x509                       \
            -config secure-data/ca.conf \
            -key secure-data/ca.key     \
            -out secure-data/ca.crt
```

In the `secure-data` directory, you should now have the following three files:

- `ca.conf` - root configuration file
- `ca.key` - root key file
- `ca.crt` - root certificate file

You can verify the root certificate by running the following `openssl x509` command.

```sh
$ openssl x509 -in secure-data/ca.crt -text -noout
```

You should see output similar to this:

```
Certificate:
    Data:
        Version: 3 (0x2)
        Serial Number:
            61:ca:24:00:c8:40:f3:4d:66:59:80:35:86:ca:b9:6f:98:b1:1c:5e
        Signature Algorithm: sha256WithRSAEncryption
        Issuer: O = Yugabyte, CN = CA for YugabyteDB
        Validity
            Not Before: Feb 14 04:40:56 2020 GMT
            Not After : Mar 15 04:40:56 2020 GMT
        Subject: O = Yugabyte, CN = CA for YugabyteDB
        Subject Public Key Info:
            Public Key Algorithm: rsaEncryption
                RSA Public-Key: (2048 bit)
                Modulus:
                    00:c9:8c:20:7d:63:ed:8d:9f:2d:f2:2e:90:34:2c:
                    79:0b:0b:77:2f:4c:88:78:63:28:db:91:6d:c4:21:
                    bd:e2:dd:14:a3:ba:e5:db:4d:b9:34:e8:74:7b:1f:
                    ff:70:a2:8c:0c:f5:df:d4:11:ae:5c:4c:1a:22:94:
                    98:4e:a7:63:ee:44:5b:c6:b7:f0:34:ef:4e:57:1a:
                    30:99:ee:f7:c9:d9:df:e9:af:ab:df:08:e3:69:d9:
                    d4:5d:8e:0c:50:7a:bf:be:7f:f0:7f:e3:20:13:d8:
                    c9:44:21:1f:05:6b:52:d3:77:b8:75:8e:78:c6:60:
                    3c:7e:9a:8a:77:b2:65:da:6c:25:7a:4a:ee:eb:4a:
                    a8:6b:43:79:ea:15:96:8b:3d:03:50:08:a4:2d:76:
                    2f:09:e3:eb:b3:f6:77:17:2a:3e:dc:9b:f8:60:cf:
                    93:f3:84:6a:19:b0:64:4a:0f:47:51:c9:47:0f:20:
                    5d:cd:af:1e:5d:65:36:0f:b0:44:c3:eb:9a:63:44:
                    dd:ac:25:f8:f4:60:6c:9b:72:46:6d:18:c3:94:7d:
                    b5:d9:89:79:e1:39:dd:4f:01:26:b2:da:c1:ac:af:
                    85:d9:cc:a7:02:65:2a:d6:06:47:cc:11:72:cc:d6:
                    92:45:c0:64:43:4c:13:07:d1:6f:38:8e:fe:db:1e:
                    5e:e5
                Exponent: 65537 (0x10001)
        X509v3 extensions:
            X509v3 Key Usage: critical
                Digital Signature, Non Repudiation, Key Encipherment, Certificate Sign
            X509v3 Basic Constraints: critical
                CA:TRUE, pathlen:1
    Signature Algorithm: sha256WithRSAEncryption
         9e:d1:41:36:63:78:4b:e4:57:f2:bd:23:c4:4b:e1:64:e8:c0:
         e3:e1:30:c5:2b:dd:b0:c2:99:ca:86:cb:85:70:6f:29:4c:b0:
         3e:ba:76:af:87:22:a3:64:1f:3e:4f:69:74:8b:a3:b3:e0:71:
         12:aa:0b:28:85:0a:45:40:7b:a5:d1:42:cd:51:bc:85:6a:53:
         16:69:89:78:85:bd:46:9d:1a:ca:19:14:de:72:e4:5c:91:51:
         58:99:b5:83:97:a5:63:dc:b9:7a:05:1e:a9:a7:5f:42:e1:12:
         4e:2b:e1:98:e5:31:14:b5:64:5f:66:bc:13:b8:19:ca:9c:ad:
         12:44:f8:21:3b:ef:0d:ca:9b:c4:04:d6:d7:93:d2:83:87:79:
         2a:2d:dc:de:4c:ad:30:cf:10:de:05:24:52:91:31:fd:cc:d6:
         cb:3b:ba:73:8f:ae:0d:97:f0:e4:aa:ca:76:c0:15:3c:80:7d:
         3a:d8:28:3c:91:bc:19:c8:5c:cd:94:49:31:23:ae:08:e5:9a:
         ce:62:6a:53:08:38:6d:0f:b4:fd:e9:66:8c:fb:cd:be:a0:01:
         b4:9d:39:57:58:6c:b3:8e:25:e3:86:24:13:59:d6:a0:d2:f0:
         15:1e:8c:24:44:5b:3a:db:1c:ef:60:70:24:58:df:56:99:aa:
         22:78:12:d6
```

## Copy the root certificate to each node directory

Copy the generated root certificate file (`ca.crt`) to all three node directories.

```sh
$ cp secure-data/ca.crt 127.0.0.1
$ cp secure-data/ca.crt 127.0.0.2
$ cp secure-data/ca.crt 127.0.0.3
```

## Generate key and certificate files for each node

Now you can generate the node key `node.key` and node certificate `node.crt` for each node.

### Generate configuration for each node

Repeat the steps in this section once for each node.
The IP address of each node is `<node-ip-address>`

1. Generate a configuration file (`node.conf`) for a node, using the node's IP address (`<node-ip-address>`) as the directory name.

```sh
$ cat > <node-ip-address>/node.conf
```

2. Add the following sample configuration content (use as-is, or customize as needed).

{{< note title="Note" >}}

Remember to replace the `<node-ip-address>` entry in the example configuration file below with the IP address for each node.

{{< /note >}}

```sh
#################################
# Example node configuration file
#################################

[ req ]
prompt=no
distinguished_name = my_distinguished_name

[ my_distinguished_name ]
organizationName = Yugabyte
# Required value for commonName, do not change.
commonName = <node-ip-address>
```

3. After pasting the content in step 2 and replacing `<node-ip-address>` with the node IP address, save and close the file by entering `Ctl+D`.

Repeat these three steps for each of the three nodes. You should then have a copy of `node.conf` in the `127.0.0.1`, `127.0.0.2`, and `127.0.0.3` directories.

### Generate private key files for each node

For each of the three nodes, generate the node private key by running the following command, replacing `<node-ip-address>` with the node IP address.

{{< note title="Note" >}}

For YugabyteDB to recognize the file, it must be of the format `node.<commonName>.key`. In this example,
you are using the `<node-ip-address>` for the `<commonName>`, so the file names should be `node.127.0.0.1.key`,
`node.127.0.0.2.key`, and `node.127.0.0.3.key`.

{{< /note >}}

```sh
$ openssl genrsa -out <node-ip-address>/node.<node-ip-address>.key
$ chmod 400 <node-ip-address>/node.<node-ip-address>.key
```

### Generate the node certificates

Next, you need to generate the node certificate. This has two steps. First, create the certificate signing request (CSR) for each node, changing `<node-ip-address>`to the node IP address.

```sh
$ openssl req -new \
              -config <node-ip-address>/node.conf \
              -key <node-ip-address>/node.<node-ip-address>.key \
              -out <node-ip-address>/node.csr
```

Sign each node CSR using the root key (`ca.key`) and root certificate (`ca.crt`). Run the following `openssl ca` command, changing `<node-ip-address>`to the node IP address.

```sh
$ openssl ca -config secure-data/ca.conf \
             -keyfile secure-data/ca.key \
             -cert secure-data/ca.crt \
             -policy my_policy \
             -out <node-ip-address>/node.<node-ip-address>.crt \
             -outdir <node-ip-address>/ \
             -in <node-ip-address>/node.csr \
             -days 3650 \
             -batch
```

For the `127.0.0.1` node, you should see the following output.

```
Using configuration from secure-data/ca.conf
Check that the request matches the signature
Signature ok
The Subject's Distinguished Name is as follows
organizationName      :ASN.1 12:'Yugabyte'
commonName            :ASN.1 12:'127.0.0.1'
Certificate is to be certified until Feb 11 04:53:11 2030 GMT (3650 days)

Write out database with 1 new entries
Data Base Updated
```

{{< note title="Note" >}}

Each node key and certificate should use the `node.<commonName>.[crt | key]` naming format.

{{< /note >}}

You can verify the signed certificate for each of the nodes by running the following `openssl verify` command:

```sh
$ openssl verify -CAfile secure-data/ca.crt <node-ip-address>/node.<node-ip-address>.crt
```

You should see the following output, displaying the node IP address:

```
X.X.X.X/node.X.X.X.X.crt: OK
```

## Copy configuration files to the nodes

The files needed for each node are:

- `ca.crt`
- `node.<commonName>.crt` (Example: `node.127.0.0.1.crt`)
- `node.<commonName>.key` (Example: `node.127.0.0.1.key`)

You can remove all other files in the node directories as they are unnecessary.

Upload the necessary information to each target node.

Create the directory that will contain the configuration files.

```sh
$ ssh <username>@<node-ip-address> mkdir ~/certs
```

Copy all the configuration files into the above directory by running the following commands, changing `<node-ip-address>`to match the node IP address.

```sh
$ scp <node-ip-address>/ca.crt <user>@<node-ip-address>:~/certs/<node-ip-address>
```

```sh
$ scp <node-ip-address>/node.<node-ip-address>.crt <user>@<node-ip-address>:~/certs/<node-ip-address>
```

```sh
$ scp <node-ip-address>/node.<node-ip-address>.key <user>@<node-ip-address>:~/certs/<node-ip-address>
```

You can now delete, or appropriately secure, the directories you created for the nodes on the local machine.
