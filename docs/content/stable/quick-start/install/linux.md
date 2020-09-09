---
title: Install YugabyteDB on Linux
headerTitle: 1. Install YugabyteDB
linkTitle: 1. Install YugabyteDB
description: Download and install YugabyteDB on Linux (CentOS or Ubuntu) in less than five minutes.
aliases:
  - /quick-start/install/
block_indexing: true
menu:
  stable:
    parent: quick-start
    name: 1. Install YugabyteDB
    identifier: install-2-linux
    weight: 110
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/quick-start/install/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/stable/quick-start/install/linux" class="nav-link active">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/stable/quick-start/install/docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="/stable/quick-start/install/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

## Prerequisites

1. One of the following operating systems

    - <i class="icon-centos"></i> CentOS 7

    - <i class="icon-ubuntu"></i> Ubuntu 16.04 or later

2. Verify that you have Python 2 or 3 installed.

    ```sh
    $ python --version
    ```

    ```
    Python 3.7.3
    ```

3. `wget` or `curl` is available.

    The instructions use the `wget` command to download files. If you prefer to use `curl`, you can replace `wget` with `curl -O`.

    To install `wget`:

    - CentOS: `yum install wget`
    - Ubuntu: `apt install wget`

    To install `curl`:

    - CentOS: `yum install curl`
    - Ubuntu: `apt install curl`

4. Each tablet maps to its own file, so if you experiment with a few hundred tables and a few tablets per table, you can soon end up creating a large number of files in the current shell. Make sure to [configure ulimit values](../../../deploy/manual-deployment/system-config#ulimits).

## Download YugabyteDB

1. Download the YugabyteDB package using the following `wget` command.

    ```sh
    $ wget https://downloads.yugabyte.com/yugabyte-2.3.0.0-linux.tar.gz
    ```

2. Extract the package and then change directories to the YugabyteDB home.

    ```sh
    $ tar xvfz yugabyte-2.3.0.0-linux.tar.gz && cd yugabyte-2.3.0.0/
    ```

## Configure YugabyteDB

To configure YugabyteDB, run the following shell script.

```sh
$ ./bin/post_install.sh
```

{{<tip title="Next step" >}}

[Create a local cluster](../../create-local-cluster/linux)

{{< /tip >}}
