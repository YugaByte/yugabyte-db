An “airgapped” host has either no or a restricted path to inbound or outbound Internet traffic at all.

## Prerequisites

### 1. Whitelist endpoints

In order to install Replicated and YugaWare on a host with no Internet connectivity at all, you have to first download the binaries on a machine that has Internet connectivity and then copy the files over to the appropriate host. In case of restricted connectivity, the following endpoints have to be whitelisted to ensure that they are accessible from the host marked for installation.

```sh
https://downloads.yugabyte.com
https://download.docker.com
```

### 2. Install Docker Engine

A supported version of docker-engine (currently 1.7.1 to 17.03.1-ce) needs to be installed on the host. If you do not have docker-engine installed, follow the instructions [here](https://help.replicated.com/docs/kb/supporting-your-customers/installing-docker-in-airgapped/) to first install docker-engine on an airgapped host. After docker-engine is installed, perform the following steps to install Replicated and then YugaWare.

## Step 1. Install Replicated

On a machine connected to the Internet, perform the following steps.

Make a directory for downloading the binaries.

```sh
$ sudo mkdir /opt/downloads
```

Change the owner user for the directory.

```sh
$ sudo chown -R ubuntu:ubuntu /opt/downloads
```

Change to the directory.

```sh
$ cd /opt/downloads
```

Get the replicated binary.

```sh
$ wget https://downloads.yugabyte.com/replicated.tar.gz
```

Get the yugaware binary where the last 4 digits refer to the version of the binary. Change this number as needed.

```sh
$ wget https://downloads.yugabyte.com/yugaware-1.2.6.0.airgap
```

Change to the directory.

```sh
$ cd /opt/downloads
```

Expand the replicated binary.

```sh
$ tar xzvf replicated.tar.gz
```

Install replicated (yugaware will be installed via replicated ui after replicated install completes) pick `eth0` network interface in case multiple ones show up.

```sh
$ cat ./install.sh | sudo bash -s airgap
```

After replicated install completes, make sure it is running.

```sh
$ sudo docker ps
```

You should see an output similar to the following.

![Replicated successfully installed](/images/replicated/replicated-success.png)

Next step is install YugaWare as described in the [section below](#step-2-install-yugaware-via-replicated).

## Step 2. Install YugaWare using Replicated

### Set up HTTPS for Replicated

Launch Replicated UI by going to [http://yugaware-host-public-ip:8800](http://yugaware-host-public-ip:8800). The warning shown next states that the connection to the server is not private (yet). We will address this warning as soon as we setup HTTPS for the Replicated Admin Console in the next step. Click Continue to Setup and then ADVANCED to bypass this warning and go to the Replicated Admin Console.

![Replicated SSL warning](/images/replicated/replicated-warning.png)

You can provide your own custom SSL certificate along with a hostname.

![Replicated HTTPS setup](/images/replicated/replicated-https.png)

The simplest option is use a self-signed cert for now and add the custom SSL certificate later. Note that you will have to connect to the Replicated Admin Console only using IP address (as noted below).

![Replicated Self Signed Cert](/images/replicated/replicated-selfsigned.png)

### Upload license file

Now upload the Yugabyte license file received from [Yugabyte](https://www.yugabyte.com/platform/#request-trial-form).

![Replicated License Upload](/images/replicated/replicated-license-upload.png)

Two options to install YugaWare are presented.

![Replicated License Airgapped Install](/images/replicated/replicated-license-airgapped-install-option.png)

![Replicated License Airgapped Path](/images/replicated/replicated-license-airgapped-path.png)

![Replicated License Airgapped Progress](/images/replicated/replicated-license-airgapped-progress.png)

### Secure Replicated

The next step is to add a password to protect the Replicated Admin Console (note that this Admin Console is for Replicated and is different from YugaWare, the Admin Console for YugabyteDB).

![Replicated Password](/images/replicated/replicated-password.png)

### Pre-flight checks

Replicated will perform a set of pre-flight checks to ensure that the host is setup correctly for the YugaWare application.

![Replicated Checks](/images/replicated/replicated-checks.png)

Clicking Continue above will bring us to YugaWare configuration.

In case the pre-flight check fails, review the [Troubleshoot Yugabyte Platform](../../../troubleshoot/enterprise-edition/) section below to identify the resolution.
