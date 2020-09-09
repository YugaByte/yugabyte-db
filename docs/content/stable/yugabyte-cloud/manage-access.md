---
title: Manage database access
linkTitle: Manage database access
description: Manage database access
headcontent:
image: /images/section_icons/deploy/enterprise.png
beta: /stable/faq/general/#what-is-the-definition-of-the-beta-feature-tag
aliases:
  - /stable/deploy/yugabyte-cloud/manage-access/
block_indexing: true
menu:
  stable:
    identifier: manage-access
    parent: yugabyte-cloud
    weight: 644
isTocNested: true
showAsideToc: true
---

Use the Database Access page to manage YugabyteDB users. This page displays a user list that includes username, cluster, authenication method, and YugabyteDB roles. Here's an example of the page:

![Database Access page](/images/deploy/yugabyte-cloud/database-access.png)

## Add a user

You can add a YugabyteDB user with admin access privileges for the Yugabyte database.

To add a user:

1. Click **ADD NEW USER**. The **Add New User** dialog appears.
2. Enter a username. Quotes or apostrophes are not permitted.
3. Enter a password. Optionally, to generate a secure password, click **Autogenerate Secure Password**. You can click **Show** in the password field to see the password.
4. Click **Add User** to add the user.

![Add New User dialog](/images/deploy/yugabyte-cloud/add-new-user.png)

After adding a user, a user's password cannot be changed on this page.

## Delete a user

To delete a user, click **Delete** next to the username in the list. The user is deleted without a warning.

## View user details

The user details include a note that the credentials give admin access to the Yugabyte database, the username, and the password. 
Click **INFO** under **Actions** for the user whose details you want to see.
To see the password, click **Show password**. Click **Close** when finished.

![User Details dialog](/images/deploy/yugabyte-cloud/user-details.png)

