---
title: Contribute to YugabyteDB
headerTitle: Contribute to YugabyteDB
linkTitle: Contribute
description: Contribute to the YugabyteDB code and documentation.
image: /images/section_icons/index/quick_start.png
headcontent: Contribute code and docs to improve YugabyteDB.
type: page
section: YUGABYTEDB CORE
menu:
  latest:
    identifier: contribute
    weight: 2800
---

We are big believers in open source. [YugabyteDB](https://github.com/yugabyte/yugabyte-db) is distributed under the Apache v2.0 license, which is very permissive open source license. We value external contributions and fully welcome them! We accept contributions as GitHub pull requests. This page contains everything you need to get you going quickly.

## Learn about the architecture

There are a number of resources to get started, here is a recommended reading list:

* Go through the [architecture section of the docs](../architecture/). It will give an overview of the high level components in the database.

## Sign the CLA

When your first contribution is submitted as a pull request, you will be given a link to sign the [Yugabyte Individual Contributor License Agreement (CLA)](https://cla-assistant.io/yugabyte/yugabyte-db).

## Pick an area

There are a few areas you can get involved in depending on your interests.

### Core database

This is the C++ code and the unit tests that comprise the core of YugabyteDB. You can [follow the steps outlined here](core-database/checklist) to get going with steps such as building the codebase and running the unit tests.

### Docs

[YugabyteDB documentation](/) uses the Hugo framework. There are two types of docs issues - infrastructure enhancements and adding content. You can [follow the steps outlined here](https://github.com/yugabyte/yugabyte-db/tree/master/docs) to run a local version of the docs site. Next, look at the [contributing guide](https://github.com/yugabyte/yugabyte-db/blob/master/docs/CONTRIBUTING.md) to make your changes and contribute them.

## Find an issue

Issues are tagged with other useful labels as described below.

| Key labels         |  Comments      |
| ------------------ | -------------- |
| `bug`              | Bugs can have a small or large scope, so make sure to understand the bug. |
| `docs`             | An issue related to the docs infrastructure, or content that needs to be added to the docs. |
| `enhancement`      | An enhancement is often done to an existing feature, and is usually limited in scope. |
| `good first issue` | A great first issue to work on as your initial contribution to YugabyteDB. |
| `help wanted`      | Issues that are very useful and relatively standalone, but not actively being worked on. |
| `new feature`      | An new feature does not exist. New features can be complex additions to the existing system, or standalone pieces. |
| `question`         | A question that needs to be answered. |

* If you are just starting out contributing to YugabyteDB, first off welcome and thanks! Look for issues labelled [good first issue](https://github.com/yugabyte/yugabyte-db/issues?q=is%3Aopen+is%3Aissue+label%3A%22good+first+issue%22).

* If you have already contributed and are familiar, then look for issues with the [help wanted](https://github.com/yugabyte/yugabyte-db/issues?q=is%3Aopen+is%3Aissue+label%3A%22help+wanted%22) label.

{{< note title="Note" >}}

If you want to work on a feature and are not sure which feature to pick, or how to proceed with the one you picked - do not hesitate to ask for help in our [Slack chat](https://www.yugabyte.com/slack) or our [community forum](https://forum.yugabyte.com/).

{{< /note >}}
