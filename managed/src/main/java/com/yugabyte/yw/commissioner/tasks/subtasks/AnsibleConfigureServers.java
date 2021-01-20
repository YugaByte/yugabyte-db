/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.yugabyte.yw.commissioner.tasks.UpgradeUniverse;
import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.common.CallHomeManager.CollectionLevel;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.Alert;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.Universe.UniverseUpdater;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.stream.Collectors;
import java.util.HashMap;
import java.util.Map;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

public class AnsibleConfigureServers extends NodeTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(AnsibleConfigureServers.class);

  public static class Params extends NodeTaskParams {
    public UpgradeUniverse.UpgradeTaskType type = UpgradeUniverse.UpgradeTaskType.Everything;
    public String ybSoftwareVersion = null;

    // Optional params.
    public boolean isMasterInShellMode = false;
    public boolean isMaster = false;
    public boolean enableYSQL = false;
    public boolean enableYEDIS = false;
    public boolean enableNodeToNodeEncrypt = false;
    public boolean enableClientToNodeEncrypt = false;
    public boolean allowInsecure = true;
    public Map<String, String> gflags = new HashMap<>();
    public Set<String> gflagsToRemove = new HashSet<>();
    public boolean updateMasterAddrsOnly = false;
    public CollectionLevel callhomeLevel;
    // Development params.
    public String itestS3PackagePath = "";
  }

  @Override
  protected Params taskParams() {
    return (Params)taskParams;
  }

  @Override
  public void run() {
    // Execute the ansible command.
    ShellResponse response = getNodeManager().nodeCommand(
        NodeManager.NodeCommandType.Configure, taskParams());
    processShellResponse(response);

    if (taskParams().type == UpgradeUniverse.UpgradeTaskType.Everything &&
        !taskParams().updateMasterAddrsOnly) {
      // Check cronjob status if installing software.
      response = getNodeManager().nodeCommand(
          NodeManager.NodeCommandType.CronCheck, taskParams());

      // Create an alert if the cronjobs failed to be created on this node.
      if (response.code != 0) {
        Universe universe = Universe.get(taskParams().universeUUID);
        Customer cust = Customer.get(universe.customerId);
        String alertErrCode = "CRON_CREATION_FAILURE";
        String nodeName = taskParams().nodeName;
        String alertMsg = "Universe %s was successfully created but failed to " +
                          "create cronjobs on some nodes (%s)";

        // Persist node cronjob status into the DB.
        UniverseUpdater updater = new UniverseUpdater() {
          @Override
          public void run(Universe universe) {
            UniverseDefinitionTaskParams universeDetails = universe.getUniverseDetails();
            NodeDetails node = universe.getNode(nodeName);
            node.cronsActive = false;
            LOG.info(
              "Updated " + nodeName + " cronjob status to inactive from universe "
                + taskParams().universeUUID);
          }
        };
        saveUniverseDetails(updater);

        // Create new alert or update existing alert with current node name if alert already exists.
        if (Alert.exists(alertErrCode, universe.universeUUID)) {
          Alert cronAlert = Alert.get(cust.uuid, alertErrCode, universe.universeUUID);
          List<String> failedNodesList = universe.getNodes().stream()
              .map(nodeDetail -> nodeDetail.nodeName)
              .collect(Collectors.toList());
          String failedNodesString = String.join(", ", failedNodesList);
          cronAlert.update(String.format(alertMsg, universe.name, failedNodesString));
        } else {
          Alert.create(
            cust.uuid, universe.universeUUID, Alert.TargetType.UniverseType, alertErrCode,
            "Warning", String.format(alertMsg, universe.name, nodeName));
        }
      }

      // We set the node state to SoftwareInstalled when configuration type is Everything.
      // TODO: Why is upgrade task type used to map to node state update?
      setNodeState(NodeDetails.NodeState.SoftwareInstalled);
    }
  }
}
