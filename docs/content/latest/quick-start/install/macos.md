---
title: Install YugabyteDB on macOS
headerTitle: 1. Install YugabyteDB
linkTitle: 1. Install YugabyteDB
description: Download and install YugabyteDB on macOS in less than five minutes.
aliases:
  - /quick-start/install/
  - /latest/quick-start/install/
menu:
  latest:
    parent: quick-start
    name: 1. Install YugabyteDB
    identifier: install-1-macos
    weight: 110
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/install/macos" class="nav-link active">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/install/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/install/docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/install/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

## Prerequisites

1. <i class="fab fa-apple" aria-hidden="true"></i> macOS 10.12 or later.

2. Verify that you have Python 2 or 3 installed. 

    ```sh
    $ python --version
    ```

    ```
    Python 3.7.3
    ```

3. `wget` or `curl` is available.

    The instructions use the `wget` command to download files. If you prefer to use `curl` (included in macOS), you can replace `wget` with `curl -O`.

    To install `wget` on your Mac, you can run the following command if you use Homebrew:

    ```sh
    $ brew install wget
    ```

4. Each tablet maps to its own file, so if you experiment with a few hundred tables and a few tablets per table, you can soon end up creating a large number of files in the current shell. Make sure that this command shows a big enough value.

    ```sh
    $ launchctl limit maxfiles
    ```

    We recommend setting the soft and hard limits to 1048576.

    Edit `/etc/sysctl.conf`, if it exists, to include the following:

    ```sh
    kern.maxfiles=1048576
    kern.maxproc=2500
    kern.maxprocperuid=2500
    kern.maxfilesperproc=1048576
    ```

    If this file does not exist, then create the file `/Library/LaunchDaemons/limit.maxfiles.plist` and insert the following:

    ```xml
    <?xml version="1.0" encoding="UTF-8"?>
    <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
      <plist version="1.0">
        <dict>
          <key>Label</key>
            <string>limit.maxfiles</string>
          <key>ProgramArguments</key>
            <array>
              <string>launchctl</string>
              <string>limit</string>
              <string>maxfiles</string>
              <string>1048576</string>
              <string>1048576</string>
            </array>
          <key>RunAtLoad</key>
            <true/>
          <key>ServiceIPC</key>
            <false/>
        </dict>
      </plist>
    ```

    Enure that the `plist` file is owned by `root:wheel` and has permissions `-rw-r--r--`. To take effect, you need to reboot your computer or run this command:

    ```sh
    $ sudo launchctl load -w /Library/LaunchDaemons/limit.maxfiles.plist
    ```

    You might have to `unload` the service before loading it.

## Download YugabyteDB

Download the YugabyteDB `tar.gz` file using the following `wget` command.

```sh
$ wget https://downloads.yugabyte.com/yugabyte-2.5.1.0-darwin.tar.gz
```

To unpack the archive file and change to the YugabyteDB home directory, run the following command.

```sh
$ tar xvfz yugabyte-2.5.1.0-darwin.tar.gz && cd yugabyte-2.5.1.0/
```

## Configure

Some of the examples in the [Explore core features](../../../explore/) section require extra loopback addresses that allow you to simulate the use of multiple hosts or nodes.

To add six loopback addresses, run the following commands, which require `sudo` access.

```sh
sudo ifconfig lo0 alias 127.0.0.2
sudo ifconfig lo0 alias 127.0.0.3
sudo ifconfig lo0 alias 127.0.0.4
sudo ifconfig lo0 alias 127.0.0.5
sudo ifconfig lo0 alias 127.0.0.6
sudo ifconfig lo0 alias 127.0.0.7
```

**Note**: The loopback addresses do not persist upon rebooting of your Mac.

To verify that the extra loopback addresses exist, run the following command.

```sh
$ ifconfig lo0
```

You should see some output like the following:

```
lo0: flags=8049<UP,LOOPBACK,RUNNING,MULTICAST> mtu 16384
  options=1203<RXCSUM,TXCSUM,TXSTATUS,SW_TIMESTAMP>
  inet 127.0.0.1 netmask 0xff000000
  inet6 ::1 prefixlen 128
  inet6 fe80::1%lo0 prefixlen 64 scopeid 0x1
  inet 127.0.0.2 netmask 0xff000000
  inet 127.0.0.3 netmask 0xff000000
  inet 127.0.0.4 netmask 0xff000000
  inet 127.0.0.5 netmask 0xff000000
  inet 127.0.0.6 netmask 0xff000000
  inet 127.0.0.7 netmask 0xff000000
  nd6 options=201<PERFORMNUD,DAD>
```

{{<tip title="Next step" >}}

[Create a local cluster](../../create-local-cluster/macos)

{{< /tip >}}
