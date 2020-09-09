---
title: Legal information
headerTitle: Legal information
linkTitle: Legal
description: Learn about Yugabyte licenses and third party software.
image: /images/section_icons/index/quick_start.png
headcontent: Learn about YugabyteDB licenses and third party software.
type: page
section: MISC
block_indexing: true
menu:
  stable:
    identifier: legal
    weight: 3000
---

## Licenses

Source code in the [YugabyteDB repository](https://github.com/yugabyte/yugabyte-db/) is variously licensed under the Apache License 2.0 and the Polyform Free Trial License 1.0.0. A copy of each license can be found in the [licenses](https://github.com/yugabyte/yugabyte-db/tree/master/licenses) directory.

The build produces two sets of binaries:

- The entire database with all its features (including the enterprise ones) are licensed under the [Apache License 2.0](https://github.com/yugabyte/yugabyte-db/blob/master/licenses/APACHE-LICENSE-2.0.txt).
- The binaries that contain `-managed` in the artifact and help run a managed service are licensed under the [Polyform Free Trial License 1.0.0](https://github.com/yugabyte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt).

By default, the build options generate only the Apache License 2.0 binaries.

## Individual contributor license agreement (CLA)

As an open source project with a strong focus on the user community, Yugabyte welcomes contributions from individuals as GitHub pull requests. When a pull request is submitted for the first time, the contributor will be notified to sign the [YugabyteDB Individual Contributor License Agreement](https://cla-assistant.io/yugabyte/yugabyte-db).

## Third party software

Yugabyte proudly participates in the open source community and appreciates all open source contributions that are incorporated in the YugabyteDB project. For acknowledgements and a listing of third party open source software components, see [Third party software](./third-party-software).
