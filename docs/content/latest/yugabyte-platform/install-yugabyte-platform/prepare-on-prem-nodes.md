---
title: Prepare nodes (on-prem)
headerTitle: Prepare nodes (on-prem)
linkTitle: Prepare nodes (on-prem)
description: Prepare YugabyteDB nodes for on-premises deployments.
menu:
  latest:
    identifier: prepare-on-prem-nodes
    parent: install-yugabyte-platform
    weight: 79
isTocNested: false
showAsideToc: true
---

For on-premises deployments of YugabyteDB universes, you need to import nodes that can be managed by the Yugabyte Platform. This page outlines the steps required to prepare these YugabyteDB nodes for on-premises deployments.



1. Ensure that the YugabyteDB nodes conform to the requirements outlined in the [deployment checklist](https://docs.yugabyte.com/latest/deploy/checklist/). This checklist also gives an idea of [recommended instance types across public clouds](https://docs.yugabyte.com/latest/deploy/checklist/#running-on-public-clouds). 
2. Install the prerequisites and verify the system resource limits as described in [system configuration](https://docs.yugabyte.com/latest/deploy/manual-deployment/system-config).
3. Need ssh access to the machine and root access (or ability to run sudo)
    *   This sudo user can require a password but having passwordless access is desirable for simplicity and ease of use.
4. 3.Verify that you can `ssh` into this node (from your local machine if the node has a public address).


```
$ ssh -i your_private_key.pem ssh_user@node_ip
```

5. These are the actions that will be performed with sudo access.
    *   Create the `yugabyte:yugabyte` user + group
    *   Set home dir /home/yugabyte
    *   Create the `prometheus:prometheus` user + group

{{< tip title="Tip" >}}
In case you’re using the LDAP directory for managing system users, then you can pre-provision yugabyte and prometheus users 

*   `yugabyte` user should belong to the `yugabyte` group
*   Set home dir for yugabyte user (default /home/yugabyte) and ensure the directory is owned by `yugabyte:yugabyte`. The home directory will be used in the cloud provider config step
*   Prometheus username and the group can be user-defined. You will enter the custom user during the cloud provider configuration setup.
{{< /tip >}}

    * Ability to schedule Cron jobs with Crontab. Cron jobs are used for health monitoring, log file rotation, and system core files cleanup.


{{< tip title="Tip" >}}
For any 3rd party cron scheduling tools, you can add these cron entries and disable Crontab. Disabling Crontab will create alerts after the universe creation that can be ignored. But you need to ensure cron jobs are set appropriately  for the platform to work as expected. 

```
# Ansible: cleanup core files hourly
0 * * * * /home/yugabyte/bin/clean_cores.sh
# Ansible: cleanup yb log files hourly
5 * * * * /home/yugabyte/bin/zip_purge_yb_logs.sh
# Ansible: Check liveness of master
*/1 * * * * /home/yugabyte/bin/yb-server-ctl.sh master cron-check || /home/yugabyte/bin/yb-server-ctl.sh master start
# Ansible: Check liveness of tserver
*/1 * * * * /home/yugabyte/bin/yb-server-ctl.sh tserver cron-check || /home/yugabyte/bin/yb-server-ctl.sh tserver start
```

{{< /tip >}}

*   Verify that Python 2.7 is installed
*   Enable core dumps + set ulimits

    ```
      *       hard        core        unlimited
      *       soft        core        unlimited
    ```


*   Configure SSH

    1. Disable sshguard
    2. Set “UseDNS no” in /etc/ssh/sshd_config (disables reverse lookup, which is used for auth. DNS is still useable)
*   Set “vm.swappiness” to 0
*   Set mount path permissions to 0755
