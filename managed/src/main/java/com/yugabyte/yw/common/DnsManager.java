/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common;

import com.google.inject.Singleton;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;


@Singleton
public class DnsManager extends DevopsBase {
  private static final String YB_CLOUD_COMMAND_TYPE = "dns";
  public enum DnsCommandType {
    Create,
    Edit,
    Delete,
    List
  }

  @Override
  protected String getCommandType() {
    return YB_CLOUD_COMMAND_TYPE;
  }

  public ShellResponse manipulateDnsRecord(
      DnsCommandType type, UUID providerUUID, String hostedZoneId,
      String domainNamePrefix, String nodeIpCsv) {
    List<String> commandArgs = new ArrayList<>();
    commandArgs.add("--hosted_zone_id");
    commandArgs.add(hostedZoneId);
    commandArgs.add("--domain_name_prefix");
    commandArgs.add(domainNamePrefix);
    commandArgs.add("--node_ips");
    commandArgs.add(nodeIpCsv);
    return execCommand(null, providerUUID, null, type.toString().toLowerCase(),
        commandArgs, new ArrayList<>());
  }

  public ShellResponse listDnsRecord(UUID providerUUID, String hostedZoneId) {
    List<String> commandArgs = new ArrayList<>();
    commandArgs.add("--hosted_zone_id");
    commandArgs.add(hostedZoneId);
    return execCommand(null, providerUUID, null, DnsCommandType.List.toString().toLowerCase(),
        commandArgs, new ArrayList<>());
  }
}
