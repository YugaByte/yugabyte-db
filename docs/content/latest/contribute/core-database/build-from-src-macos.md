---
title: Build from source code on macOS
headerTitle: Build the source code
linkTitle: Build the source
description: Build YugabyteDB from source code on macOS.
image: /images/section_icons/index/quick_start.png
headcontent: Build the source code on macOS, CentOS, and Ubuntu.
type: page
aliases:
  - /latest/contribute/core-database/build-from-src
menu:
  latest:
    identifier: build-from-src-1-macos
    parent: core-database
    weight: 2912
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./build-from-src-macos.md" >}}" class="nav-link active">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="{{< relref "./build-from-src-centos.md" >}}" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      CentOS
    </a>
  </li>

  <li >
    <a href="{{< relref "./build-from-src-ubuntu.md" >}}" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Ubuntu
    </a>
  </li>

</ul>

{{< note title="Note" >}}

CentOS 7 is the recommended Linux distribution for development and production platform for YugabyteDB.

{{< /note >}}

## Install necessary packages

First, install [Homebrew](https://brew.sh/), if you do not already have it. Homebrew will be used to install the other required packages.

```sh
/usr/bin/ruby -e "$(
  curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
```

Install the following packages using Homebrew:

```sh
brew install autoconf automake bash ccache cmake coreutils gnu-tar libtool \
             maven ninja pkg-config pstree wget python
```

{{< note title="Note" >}}

YugabyteDB build scripts rely on Bash 4. Make sure that `which bash` outputs `/usr/local/bin/bash` before proceeding. You may need to put `/usr/local/bin` as the first directory on `PATH` in your `~/.bashrc` to achieve that.

{{< /note >}}

## Build the code

Assuming this repository is checked out in `~/code/yugabyte-db`, run the following:

```sh
cd ~/code/yugabyte-db
./yb_build.sh release
```

The command above builds the release configuration, puts the C++ binaries in `build/release-clang-dynamic-ninja`, and creates the `build/latest` symlink to that directory.

{{< tip title="Tip" >}}

You can find the binaries you just built in `build/latest` directory.

{{< /tip >}}

## Build Java code

YugabyteDB core is written in C++, but the repository contains Java code needed to run sample applications. To build the Java part, you need:

* JDK 8
* [Apache Maven](https://maven.apache.org/).

Also make sure Maven's `bin` directory is added to your `PATH` (for example, by adding to your `~/.bashrc`). See the example below (if you've installed Maven into `~/tools/apache-maven-3.6.3`)

```sh
export PATH=$HOME/tools/apache-maven-3.6.3/bin:$PATH
```

For building YugabyteDB Java code, you'll need to install Java and Apache Maven.

## Build release package
You can build a release package by executing:

```shell
$ ./yb_release
......
2020-10-27 13:55:40,856 [yb_release.py:283 INFO] Generated a package at '/Users/me/code/yugabyte-db/build/yugabyte-2.5.1.0-6ab8013159fdca00ced7e6f5d2f98cacac6a536a-release-darwin-x86_64.tar.gz'```
```
