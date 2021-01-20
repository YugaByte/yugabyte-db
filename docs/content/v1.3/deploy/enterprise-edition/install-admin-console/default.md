
YugabyteDB clusters are created and managed from YugaWare. The default option to install YugaWare on a host machine that is connected to the Internet.

## Step 1. Install Replicated

Connect to the YugaWare instance and do the following.

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

## Step 2. Install YugaWare

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

If you are asked to choose an installation type, choose `Online`.

![Replicated License Online Install](/images/replicated/replicated-license-online-install-option.png)

![Replicated License Online Progress](/images/replicated/replicated-license-progress.png)

### Secure Replicated

The next step is to add a password to protect the Replicated Admin Console (note that this Admin Console is for Replicated and is different from YugaWare, the Admin Console for YugabyteDB).

![Replicated Password](/images/replicated/replicated-password.png)

### Pre-flight checks

Replicated will perform a set of pre-flight checks to ensure that the host is setup correctly for the YugaWare application.

![Replicated Checks](/images/replicated/replicated-checks.png)

Clicking Continue above will bring us to YugaWare configuration.

In case the pre-flight check fails, see [Troubleshoot Yugabyte Platform](../../../troubleshoot/enterprise-edition/) to identify the resolution.
