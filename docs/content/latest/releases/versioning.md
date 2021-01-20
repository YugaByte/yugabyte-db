---
title: Release versioning
headerTitle: Release versioning
linkTitle: Release versioning
description: Explains the new release versioning convention for latest and stable releases.
section: RELEASES
menu:
  latest:
    identifier: versioning
    weight: 2586
isTocNested: true
showAsideToc: false
---

Starting with v2.2.0, Yugabyte follows a new release versioning convention for stable and development releases. The following sections cover the new release versioning convention and includes descriptions of stable and latest releases.

## Release versioning convention

Yugabyte follows the [semantic versioning (semver)](https://semver.org) convention for numbering release versions, modified slightly to use even and odd minor releases denote stable and development releases, respectively. Release versions follow the semantic versioning format of `MAJOR.MINOR.PATCH`, where non-negative integers are used for:

- `MAJOR` — Includes substantial changes.
- `MINOR` — Incremented when new features and changes are introduced.
  - `EVEN` — Stable minor release, intended for production deployments.
  - `ODD` — Latest minor release, intended for development and testing.
- `PATCH` — Patches in a stable release (`MAJOR.EVEN.PATCH`) include bug fixes and revisions that do not break backward compatibility. For patches in the latest release series (`MAJOR.ODD.PATCH`), new features and changes are introduced that might break backward compatibility.

Examples are included in the relevant sections below.

## Stable releases

Releases within stable release series, denoted by `MAJOR.EVEN` versioning, introduce fully tested new features and changes added since the last stable release. A stable release series is based on the previous latest release series. For example, the upcoming v2.4 stable release series will be based on the v2.3 latest release series.

Patch releases in a stable release series (`MAJOR.EVEN.PATCH`) include bug fixes and revisions that do not break backward compatibility.

{{< note title="Important" >}}

- Yugabyte supports *production deployments* on stable releases and upgrades to newer stable releases. For a list of supported stable releases, see [Current supported releases](../releases-overview/#current-supported-releases).
- For recommendations on which version to use for development and testing, see [Recommended release series for projects](../releases-overview/#recommended-releases-series-for-projects).

{{< /note >}}

## Latest releases

Releases within the latest release series, denoted by `MAJOR.ODD` versioning, are under active development and incrementally introduces new features and changes and are intended for development, testing, and proof-of-concept projects. The v2.3 latest release series will become the basis for the next v2.4 stable release series. And the v2.5 latest release series will be the next actively developed .

Patch releases in the latest release series (`MAJOR.ODD.PATCH`) introduce new features, enhancements, and fixes.

{{< note title="Note" >}}

The latest release series is not supported for production deployments.

{{< /note >}}