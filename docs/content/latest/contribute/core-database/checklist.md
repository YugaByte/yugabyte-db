---
title: Contribution checklist
headerTitle: Contribution checklist
linkTitle: Contribution checklist
description: Review the steps to start contributing code and documentation.
image: /images/section_icons/index/quick_start.png
headcontent: Checklist for contributing to the core database.
type: page
menu:
  latest:
    identifier: contribute-checklist
    parent: core-database
    weight: 2911
isTocNested: true
showAsideToc: true
---

## Step 1. Build the source

* First, clone [the YugabyteDB GitHub repo](https://github.com/yugabyte/yugabyte-db).
* Next, [build the source code](../build-from-src).
* Optionally, you may want to [run the unit tests](../run-unit-tests).

## Step 2. Start a local cluster

Having built the source, you can [start a local cluster](../../quick-start/create-local-cluster).

## Step 3. Make the change

You should now make you change, recompile the code and test out your change.

{{< note title="Note" >}}

You should read the [code style guide](https://goo.gl/Hkt5BU). This is mostly based on [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).

{{< /note >}}

## Step 4. Add unit tests

Depending on the change, you should add unit tests to make sure they do not break as the codebase is modified.

## Step 5. Re-run unit tests

Re-run the unit tests with you changes and make sure all tests pass.

## Step 6. Submit a pull request

Congratulations on the change! You should now submit a pull request for a code review and leave a message on the Slack channel. Once the code review passes, your code will get merged in.
