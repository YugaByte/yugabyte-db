---
title: Install Yugabyte Platform
headerTitle: Install Yugabyte Platform
linkTitle: Install software
description: Install the Yugabyte Platform software.
aliases:
  - /stable/yugabyte-platform/install-yugabyte-platform/install-software/
menu:
  stable:
    parent: install-yugabyte-platform
    identifier: install-software-1-default
    weight: 77
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/yugabyte-platform/install-yugabyte-platform/install-software/default" class="nav-link active">
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
    <a href="/stable/yugabyte-platform/install-yugabyte-platform/install-software/airgapped" class="nav-link">
      <i class="fas fa-unlink"></i>
      Airgapped
    </a>
  </li>

</ul>

YugabyteDB universes and clusters are created and managed using the Yugabyte Platform. The default option to install Yugabyte Platform on a host machine that is connected to the Internet.

## Install Replicated

Connect to the Yugabyte Platform instance and do the following.

1. Install Replicated.

```sh
$ curl -sSL https://get.replicated.com/docker | sudo bash
```

**NOTE**: If you are installing Replicated behind a proxy, you need to run the following:

```sh
$ curl -x http://<proxy_address>:<proxy_port> https://get.replicated.com/docker | sudo bash
```

After the Replicated installation completes, verify that it is running by running the following command:

```sh
$ sudo docker ps
```

You should see an output similar to the following:

![Replicated successfully installed](/images/replicated/replicated-success.png)

## Set up HTTPS (optional)

Launch Replicated UI by going to [http://yugaware-host-public-ip:8800](http://yugaware-host-public-ip:8800). The warning shown next states that the connection to the server is not private (yet). You will address this warning as soon after setting up HTTPS for the Replicated Admin Console in the next step. Click **Continue to Setup** and then **ADVANCED** to bypass this warning and go to the Replicated Admin Console.

![Replicated SSL warning](/images/replicated/replicated-warning.png)

You can provide your own custom SSL certificate along with a hostname.

![Replicated HTTPS setup](/images/replicated/replicated-https.png)

The simplest option is use a self-signed certificate for now and add the custom SSL certificate later. Note that you will have to connect to the Replicated Admin Console using an IP address (as noted below).

![Replicated Self Signed Cert](/images/replicated/replicated-selfsigned.png)

## Upload the license file

Now, upload the Yugabyte license file that you received from [Yugabyte](https://www.yugabyte.com/platform/#request-trial-form).

![Replicated License Upload](/images/replicated/replicated-license-upload.png)

If you are asked to choose an installation type, choose `Online`.

![Replicated License Online Install](/images/replicated/replicated-license-online-install-option.png)

![Replicated License Online Progress](/images/replicated/replicated-license-progress.png)

## Secure Replicated

The next step is to add a password to protect the Replicated Admin Console (note that this Admin Console is for Replicated and is different from Yugabyte Platform, the Admin Console for YugabyteDB).

![Replicated Password](/images/replicated/replicated-password.png)

## Preflight checks

Replicated will perform a set of preflight checks to ensure that the host is setup correctly for Yugabyte Platform.

![Replicated Checks](/images/replicated/replicated-checks.png)

Click **Continue** to configure Yugabyte Platform.

If the preflight check fails, review the [Troubleshoot Yugabyte Platform](../../../troubleshoot/) to find a resolution.
