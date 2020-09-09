---
title: Install Yugabyte Platform using Replicated
headerTitle: Install Yugabyte Platform
linkTitle: 2. Install Yugabyte Platform
description: Use Replicated to install Yugabyte Platform (aka YugaWare).
aliases:
  - /latest/deploy/enterprise-edition/admin-console/
  - /latest/deploy/enterprise-edition/install-admin-console/
  - /latest/yugabyte-platform/deploy/install-admin-console/
menu:
  latest:
    identifier: install-yp-1-default
    parent: deploy-yugabyte-platform
    weight: 670
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/yugabyte-platform/deploy/install-admin-console/default" class="nav-link active">
      <i class="fas fa-cloud"></i>
      Default
    </a>
  </li>
  <li >
    <a href="/latest/yugabyte-platform/deploy/install-admin-console/airgapped" class="nav-link">
      <i class="fas fa-unlink"></i>
      Airgapped
    </a>
  </li>
  <li>
    <a href="/latest/yugabyte-platform/deploy/install-admin-console/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>
</ul>

YugabyteDB universes and clusters are created and managed using the Yugabyte Platform. The default option to install Yugabyte Platform on a host machine that is connected to the Internet.

## Step 1. Install Replicated

Connect to the Yugabyte Platform instance and do the following.

- Install Replicated.

```sh
$ curl -sSL https://get.replicated.com/docker | sudo bash
```

**NOTE**: If you are behind a proxy, you would need to run the following:

- Install Replicated behind a proxy.

```sh
$ curl -x http://<proxy_address>:<proxy_port> https://get.replicated.com/docker | sudo bash
```

- After Replicated install completes, make sure it is running.

You can do this as shown below.

```sh
$ sudo docker ps
```

You should see an output similar to the following.

![Replicated successfully installed](/images/replicated/replicated-success.png)

## Step 2. Install Yugabyte Platform

### Set up HTTPS for Replicated

Launch Replicated UI by going to [http://yugaware-host-public-ip:8800](http://yugaware-host-public-ip:8800). The warning shown next states that the connection to the server is not private (yet). You will address this warning as soon after setting up HTTPS for the Replicated Admin Console in the next step. Click Continue to Setup and then ADVANCED to bypass this warning and go to the Replicated Admin Console.

![Replicated SSL warning](/images/replicated/replicated-warning.png)

You can provide your own custom SSL certificate along with a hostname.

![Replicated HTTPS setup](/images/replicated/replicated-https.png)

The simplest option is use a self-signed cert for now and add the custom SSL certificate later. Note that you will have to connect to the Replicated Admin Console only using IP address (as noted below).

![Replicated Self Signed Cert](/images/replicated/replicated-selfsigned.png)

### Upload license file

Now upload the Yugabyte license file received from Yugabyte Support.

![Replicated License Upload](/images/replicated/replicated-license-upload.png)

If you are asked to choose an installation type, choose `Online`.

![Replicated License Online Install](/images/replicated/replicated-license-online-install-option.png)

![Replicated License Online Progress](/images/replicated/replicated-license-progress.png)

### Secure Replicated

The next step is to add a password to protect the Replicated Admin Console (note that this Admin Console is for Replicated and is different from Yugabyte Platform, the Admin Console for YugabyteDB).

![Replicated Password](/images/replicated/replicated-password.png)

### Pre-flight checks

Replicated will perform a set of pre-flight checks to ensure that the host is setup correctly for the Yugabyte Platform application.

![Replicated Checks](/images/replicated/replicated-checks.png)

Clicking Continue above will bring us to Yugabyte Platform configuration.

In case the pre-flight check fails, review the [Troubleshoot Yugabyte Platform](../../../troubleshoot/) to identify the resolution.
