---
title: Install Yugabyte Platform software - Airgapped
headerTitle: Install Yugabyte Platform software - Airgapped
linkTitle: Install software
description: Install Yugabyte Platform software in your on-premises, airgapped environment.
menu:
  stable:
    parent: install-yugabyte-platform
    identifier: install-software-3-airgapped
    weight: 77
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/yugabyte-platform/install-yugabyte-platform/install-software/default" class="nav-link">
      <i class="fas fa-cloud"></i>
      Default
    </a>
  </li>

  <li>
    <a href="/stable/yugabyte-platform/install-yugabyte-platform/install-software/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li >
    <a href="/stable/yugabyte-platform/install-yugabyte-platform/install-software/airgapped" class="nav-link active">
      <i class="fas fa-unlink"></i>
      Airgapped
    </a>
  </li>

</ul>

## Install Replicated

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

Download the `replicated.tar.gz` file.

```sh
$ wget https://downloads.yugabyte.com/replicated.tar.gz
```

Download the `yugaware` binary. Change this number as needed.

```sh
$ wget https://downloads.yugabyte.com/yugaware-2.1.2.0-b10.airgap
```

Change to the directory.

```sh
$ cd /opt/downloads
```

Extract the `replicated` binary.

```sh
$ tar xzvf replicated.tar.gz
```

Install Replicated. If multiple options appear, select the `eth0` network interface. The `yugaware` binary will be installed using the replicated UI after the replicated installation completes.

```sh
$ cat ./install.sh | sudo bash -s airgap
```

After Replicated finishes installing, make sure it is running.

```sh
$ sudo docker ps
```

You should see an output similar to the following.

![Replicated successfully installed](/images/replicated/replicated-success.png)

Next, install Yugabyte Platform as described in step 2.

## Set up HTTPS (optional)

Launch the Replicated UI by going to [http://yugaware-host-public-ip:8800](http://yugaware-host-public-ip:8800). The warning shown next states that the connection to the server is not private (yet). We will address this warning as soon as you configure HTTPS for the Replicated Admin Console in the next step. Click **Continue to Setup** and then **ADVANCED** to bypass this warning and go to the **Replicated Admin Console**.

![Replicated SSL warning](/images/replicated/replicated-warning.png)

You can provide your own custom SSL certificate along with a hostname.

![Replicated HTTPS setup](/images/replicated/replicated-https.png)

The simplest option is use a self-signed cert for now and add the custom SSL certificate later. Note that you will have to connect to the Replicated Admin Console only using IP address (as noted below).

![Replicated Self Signed Cert](/images/replicated/replicated-selfsigned.png)

## Upload license file

Now, upload the Yugabyte license file received from [Yugabyte](https://www.yugabyte.com/platform/#request-trial-form).

![Replicated License Upload](/images/replicated/replicated-license-upload.png)

Two options to install Yugabyte Platform are presented.

![Replicated License Air-gapped Install](/images/replicated/replicated-license-airgapped-install-option.png)

![Replicated License Air-gapped Path](/images/replicated/replicated-license-airgapped-path.png)

![Replicated License Air-gapped Progress](/images/replicated/replicated-license-airgapped-progress.png)

## Secure Replicated

The next step is to add a password to protect the Replicated Admin Console (for Replicated use only and differs from the Yugabyte Platform console).

![Replicated Password](/images/replicated/replicated-password.png)

## Preflight checks

Replicated will perform a set of preflight checks to ensure that the host is set up correctly for Yugabyte Platform.

![Replicated Checks](/images/replicated/replicated-checks.png)

Clicking **Continue** above will bring you to the Yugabyte Platform configuration.

In case the preflight check fails, review the [Troubleshoot Yugabyte Platform](../../../troubleshoot/) to identify the resolution.
