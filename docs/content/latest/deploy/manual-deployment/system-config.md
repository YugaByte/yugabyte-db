---
title: System configuration
headerTitle: System configuration
linkTitle: 1. System configuration
description: Configure NTP and ulimits on your YugabyteDB cluster
aliases:
  - /deploy/manual-deployment/system-config
menu:
  latest:
    identifier: deploy-manual-deployment-system-config
    parent: deploy-manual-deployment
    weight: 611
isTocNested: true
showAsideToc: true
---

Do the following configuration steps on each of the nodes in the cluster.

## ntp

 If your instance does not have public Internet access, make sure the ntp package is installed:

```sh
$ sudo yum install -y ntp
```

{{< note title="Note" >}}
As of CentOS 8, `ntp` is no longer available and has been replaced by `chrony`. To install, run:
```sh
$ sudo yum install -y chrony
```
{{< /note >}}

## ulimits

In Linux, `ulimit` is used to limit and control the usage of system resources (threads, files, and network connections) on a per-process or per-user basis.

### Checking ulimits

Run the following command to check the `ulimit` settings.

```sh
$ ulimit -a
```

The following settings are recommended when running YugabyteDB.

```
core file size          (blocks, -c) unlimited
data seg size           (kbytes, -d) unlimited
scheduling priority             (-e) 0
file size               (blocks, -f) unlimited
pending signals                 (-i) 119934
max locked memory       (kbytes, -l) 64
max memory size         (kbytes, -m) unlimited
open files                      (-n) 1048576
pipe size            (512 bytes, -p) 8
POSIX message queues     (bytes, -q) 819200
real-time priority              (-r) 0
stack size              (kbytes, -s) 8192
cpu time               (seconds, -t) unlimited
max user processes              (-u) 12000
virtual memory          (kbytes, -v) unlimited
file locks                      (-x) unlimited
```

### Setting system-wide ulimits

You can change values by substituting the `-n` option for any possible value in the output of `ulimit -a`. Issue a command in the following form to change a `ulimit` setting.

```sh
$ ulimit -n <value>
```

```sh
-f (file size): unlimited
-t (cpu time): unlimited
-v (virtual memory): unlimited [1]
-l (locked-in-memory size): unlimited
-n (open files): 64000
-m (memory size): unlimited [1] [2]
-u (processes/threads): 64000
```

{{< note title="Note" >}}

- After changing a ulimit setting, the YB-Master and YB-TServer servers must be restarted in order for the new settings to take effect. Check the `/proc/<process pid>` file to see the current settings.
- Changes made using ulimit may revert following a system restart depending on the system configuration.

{{< /note >}}

These settings should be applied permanently by adding the following in `/etc/security/limits.conf`.

```
*                -       core            unlimited
*                -       data            unlimited
*                -       fsize           unlimited
*                -       sigpending      119934
*                -       memlock         64
*                -       rss             unlimited
*                -       nofile          1048576
*                -       msgqueue        819200
*                -       stack           8192
*                -       cpu             unlimited
*                -       nproc           12000
*                -       locks           unlimited
```

On CentOS, /etc/security/limits.d/20-nproc.conf must also be configured to match.

```
*          soft    nproc     12000
```

{{< note title="Note" >}}

If you're using [systemd](https://systemd.io/) to start the processes, and the ulimits are not propagated, you
 must add them also in the `Service` section in the configuration file.

```
[Unit]
.....

[Service]
.....
ulimits options here

[Install]
.....
```

The mappings of ulimit options with values are:

Data type | ulimit equivalent | Value |
----------|-------------|-----|
Directive       | ulimit equivalent    | Value |
LimitCPU=       | ulimit -t            | infinity |
LimitFSIZE=     | ulimit -f            | infinity |
LimitDATA=      | ulimit -d            | infinity |
LimitSTACK=     | ulimit -s            | 8192 |
LimitCORE=      | ulimit -c            | infinity |
LimitRSS=       | ulimit -m            | infinity |
LimitNOFILE=    | ulimit -n            | 1048576  |
LimitAS=        | ulimit -v            | infinity |
LimitNPROC=     | ulimit -u            | 12000  |
LimitMEMLOCK=   | ulimit -l            | 64 |
LimitLOCKS=     | ulimit -x            | infinity |
LimitSIGPENDING=| ulimit -i            | 119934 |
LimitMSGQUEUE=  | ulimit -q            | 819200 |
LimitNICE=      | ulimit -e            | 0 |
LimitRTPRIO=    | ulimit -r            | 0 |

If a ulimit is set to `unlimited`, set it to `infinity` in the systemd config.

{{< /note >}}

{{< note title="Note" >}}

After changing a `ulimit` setting in `/etc/security/limits.conf`, you will need to log out and back in. To update system processes, you may need to restart.

{{< /note >}}

{{< note title="Note" >}}
If you're using a desktop-distribution, such as ubuntu-desktop, the settings above may not suffice.
The OS needs additional steps to change `ulimit` for gui login. In the case of ubuntu-desktop:

In `/etc/systemd/user.conf` and `/etc/systemd/system.conf`, add at the end of file `DefaultLimitNOFILE=64000`.

Something similar may be needed for other distributions.
{{< /note >}}


## transparent hugepages

Transparent hugepages should be enabled for optimal performance. By default, they are enabled.

You can check with the following command:


```sh
$ cat /sys/kernel/mm/transparent_hugepage/enabled
[always] madvise never
```

It is generally not necessary to adjust the kernel command line if the above value is correct.

However, if the value is set to "madvise" or "never", you should modify your kernel command line to set transparent hugepages to "always".


You should consult your operating system docs to determine the best way to modify a kernel command line argument for your operating system, but on RHEL or Centos 7 or 8, using grub2, the following steps are one solution:


Append "transparent_hugepage=always" to `GRUB_CMDLINE_LINUX` in `/etc/default/grub`

```sh
GRUB_CMDLINE_LINUX="rd.lvm.lv=rhel/root rd.lvm.lv=rhel/swap ... transparent_hugepage=always"
```


Rebuild `/boot/grub2/grub.cfg` using grub2-mkconfig.

Please ensure to take a backup of the existing `/boot/grub2/grub.cfg` before rebuilding.

On BIOS-based machines:

```sh
# grub2-mkconfig -o /boot/grub2/grub.cfg
```
On UEFI-based machines:

```sh
# grub2-mkconfig -o /boot/efi/EFI/redhat/grub.cfg
```


Reboot the system.
