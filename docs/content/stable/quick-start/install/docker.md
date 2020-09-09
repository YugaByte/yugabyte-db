---
title: Install YugabyteDB on Docker
headerTitle: 1. Install YugabyteDB
linkTitle: 1. Install YugabyteDB
description: Download and install YugabyteDB on Docker in less than five minutes.
aliases:
  - /quick-start/install/
block_indexing: true
menu:
  stable:
    parent: quick-start
    name: 1. Install YugabyteDB
    identifier: install-3-docker
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
    <a href="/stable/quick-start/install/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/stable/quick-start/install/docker" class="nav-link active">
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

{{< note title="Note" >}}

The Docker option to run local clusters is recommended only for advanced Docker users. This is because running stateful apps like YugabyteDB in Docker is more complex and error-prone than stateless apps.

{{< /note >}}

## Prerequisites

You must have the Docker runtime installed on your localhost. Follow the links below to download and install Docker if you have not done so already.

<i class="fab fa-apple" aria-hidden="true"></i> [Docker for Mac](https://store.docker.com/editions/community/docker-ce-desktop-mac)

<i class="fab fa-centos"></i> [Docker for CentOS](https://store.docker.com/editions/community/docker-ce-server-centos)

<i class="fab fa-ubuntu"></i> [Docker for Ubuntu](https://store.docker.com/editions/community/docker-ce-server-ubuntu)

<i class="icon-debian"></i> [Docker for Debian](https://store.docker.com/editions/community/docker-ce-server-debian)

<i class="fab fa-windows" aria-hidden="true"></i> [Docker for Windows](https://store.docker.com/editions/community/docker-ce-desktop-windows)

## Install

Pull the YugabyteDB container.

```sh
$ docker pull yugabytedb/yugabyte
```

{{<tip title="Next step" >}}

[Create a local cluster](../../create-local-cluster/docker)

{{< /tip >}}
