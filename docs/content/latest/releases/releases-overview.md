---
title: Releases overview
headerTitle: Releases overview
linkTitle: Releases overview
description: An overview of the latest and current stable releases, and upcoming releases.
section: RELEASES
menu:
  latest:
    identifier: releases-overview
    weight: 2585
isTocNested: true
showAsideToc: false
---

{{< note title="Important" >}}

As of v2.2, Yugabyte follows a new release versioning convention for YugabyteDB and Yugabyte Platform releases.  For details, see [Release versioning](../versioning).

{{< /note >}}

## Current supported releases

The following release series include:

- [Stable](../versioning/#stable-releases): Supported for production deployments.
- [Latest](../versioning/#latest-releases): Supported for development and testing only.

For details about the differences between the stable and latest release series, see [Release versioning](../versioning).

### Release support policy

Support for YugabyteDB stable release series includes:

- **Maintenance support:** For at least 1 year from the minor release date, Yugabyte will provide Updates for such release.
- **Extended support:** Following the maintenance support period, Yugabyte will provide Updates for at least an
additional 180 days subject to the following guidelines:
  - Updates and Upgrades will not be made to the minor release.
  - Yugabyte will direct Customers to existing Updates and workarounds applicable
to the reported case.
  - Yugabyte may direct Customers to Upgrade to a current release if a workaround
does not exist.
- **End of Life (EOL):** Yugabyte will post publicly on its website a notice of End of Life (EOL) for the affected
Software and the timeline for discontinuing Support Services.

For details, see [Yugabyte Services Policy](https://www.yugabyte.com/support-policy/).

### Release support timelines

| Release series                                                                  | Released     | End of maintenance support | End of Life (EOL)          |
| :-------------------------------------------------------------------- | :----------- | :----------------------- | :----------- |
| [v2.5](../whats-new/latest-releases) ![LATEST](/images/releases/latest.png)            | Nov 12, 2020 | ---                      | ---          |
| [v2.3](../whats-new/latest-releases)            | Sep 08, 2020 | Sep 08, 2021                      | Sep 08, 2022          |
| [v2.2](../whats-new/stable-releases) ![CURRENT STABLE](/images/releases/current-stable.png)               | Jul 15, 2020 | Jul 15, 2021             | Jan 15, 2022 |
| [v2.1](../earlier-releases/v2.1.0)                                               | Feb 25, 2020 | Feb 25, 2021             | Aug 08, 2021 |
| [v2.0](../earlier-releases/v2.0.0)                                               | Sep 17, 2019 | Sep 17, 2020             | Mar 03, 2021 |
| [v1.3](../earlier-releases/v1.3.0)                                               | Jul 15, 2019 | Jul 15, 2020             | Jan 15, 2021 |

**Note:** Supported stable release series include v2.1, v2.0, and v1.3, which were released prior to the new release versioning convention.

## Upcoming release series

The table below includes tentative release dates for upcoming release series, subject to change.

For information on key features planned for the upcoming releases, visit [Current roadmap](https://github.com/yugabyte/yugabyte-db#current-roadmap).

| Release series | Planned release   |
| :------------- | :---------------- |
| v2.4 (stable)   | Jan 2021          |
| v2.5            | Nov 2020          |
| v2.6 (stable)   | Apr 2021          |
| v2.7            | Feb 2021          |

## Recommended release series for projects

To ensure that your production go-live uses the most up-to-date stable release, follow this guideline.

| Production go-live  | Recommended release series |
| :------------------ | :------------------------- |
| < 3 months          | v2.2 (current stable)      |
| > 3 months          | v2.5 (latest)              |

If your production go-live is more than three months from now, start your development and testing with the latest release series. By the time your production is ready to go live, the current stable release series, based on the latest release series you used for development and testing, will be available. By following this guideline, you ensure that your application is developed with the latest available features, enhancements, and fixes.
