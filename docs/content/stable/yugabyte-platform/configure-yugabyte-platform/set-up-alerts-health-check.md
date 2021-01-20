---
title: Configure alerts and health checking
headerTitle: Configure the alerts and health checking
linkTitle: Configure alerts and health checking
description: Configure the initial alerts and health checking.
menu:
  stable:
    identifier: set-up-alerts-health-checking
    parent: configure-yugabyte-platform
    weight: 40
isTocNested: true
showAsideToc: true
---

To help you stay aware of potential deployment issues, the Yugabyte Platform has the capability to check on each individual universe for several types of issues and proactively send out email alerts when problems arise.

## Configure alerts and health checking

To configure health checking, go to your profile page in the Yugabyte Platform console by clicking the top-right drop-down list featuring your account email and then select the **Profile** option.

![Profile Dropdown](/images/ee/health/profile-button.png)

You should see something like the following:

![Alerting Controls](/images/ee/health/alerting-controls.png)

Under the **Alerting controls**, you can::

- Add email addresses to send alerts to. Use comma-separated values.
- Enable or disable sending the same alerts back to the Yugabyte support team.

Setting at least one email address or enabling sending to Yugabyte will enable the feature and subsequently begin to track the health of your universes. There are two modes of operation:

- Every 5 minutes, the background checker goes to every universe and performs a set of checks on each node. If any checks fail, the **Health** tab of the universe highlights the errors and sends an alert message by email to all of the email addresses.
- Every 12 hours, whether or not there are errors, a status email message is sent out to ensure that health checking is occurring.

Both timing options are hard-coded, but will become user-configurable knobs.
