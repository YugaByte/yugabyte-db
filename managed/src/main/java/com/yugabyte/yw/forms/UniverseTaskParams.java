// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.forms;

import io.ebean.annotation.EnumValue;

import java.util.UUID;
import java.util.Set;

import com.yugabyte.yw.common.kms.util.AwsEARServiceUtil.KeyType;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.DeviceInfo;

public class UniverseTaskParams extends AbstractTaskParams {
  public static class EncryptionAtRestConfig {
    public enum OpType {
      @EnumValue("ENABLE")
      ENABLE,
      @EnumValue("DISABLE")
      DISABLE,
      @EnumValue("UNDEFINED")
      UNDEFINED;
    }

    // Whether a universe is currently encrypted at rest or not
    public boolean encryptionAtRestEnabled;

    // The KMS Configuration associated with the encryption keys being used on this universe
    public UUID kmsConfigUUID;

    // Whether to enable/disable/rotate universe key/encryption at rest
    public OpType opType;

    // Whether to generate a data key or just retrieve the CMK arn
    public KeyType type;

    public EncryptionAtRestConfig() {
      this.encryptionAtRestEnabled = false;
      this.kmsConfigUUID = null;
      this.type = KeyType.DATA_KEY;
      this.opType = OpType.UNDEFINED;
    }

    public EncryptionAtRestConfig(EncryptionAtRestConfig config) {
      this.encryptionAtRestEnabled = config.encryptionAtRestEnabled;
      this.kmsConfigUUID = config.kmsConfigUUID;
      this.type = config.type;
      this.opType = config.opType;
    }

    public EncryptionAtRestConfig clone() {
      return new EncryptionAtRestConfig(this);
    }
  }

  public static class CommunicationPorts {
    public CommunicationPorts() {
      // Set default port values.
      exportToCommunicationPorts(this);
    }

    // Ports that are customizable universe-wide.
    public int masterHttpPort;
    public int masterRpcPort;
    public int tserverHttpPort;
    public int tserverRpcPort;
    public int redisServerHttpPort;
    public int redisServerRpcPort;
    public int yqlServerHttpPort;
    public int yqlServerRpcPort;
    public int ysqlServerHttpPort;
    public int ysqlServerRpcPort;
    public int nodeExporterPort;

    public static CommunicationPorts exportToCommunicationPorts(NodeDetails node) {
      return exportToCommunicationPorts(new CommunicationPorts(), node);
    }

    public static CommunicationPorts exportToCommunicationPorts(CommunicationPorts portsObj) {
      return exportToCommunicationPorts(portsObj, new NodeDetails());
    }

    public static CommunicationPorts exportToCommunicationPorts(
      CommunicationPorts portsObj,
      NodeDetails node
    ) {
      portsObj.masterHttpPort = node.masterHttpPort;
      portsObj.masterRpcPort = node.masterRpcPort;
      portsObj.tserverHttpPort = node.tserverHttpPort;
      portsObj.tserverRpcPort = node.tserverRpcPort;
      portsObj.redisServerHttpPort = node.redisServerHttpPort;
      portsObj.redisServerRpcPort = node.redisServerRpcPort;
      portsObj.yqlServerHttpPort = node.yqlServerHttpPort;
      portsObj.yqlServerRpcPort = node.yqlServerRpcPort;
      portsObj.ysqlServerHttpPort = node.ysqlServerHttpPort;
      portsObj.ysqlServerRpcPort = node.ysqlServerRpcPort;
      portsObj.nodeExporterPort = node.nodeExporterPort;

      return portsObj;
    }

    public static void setCommunicationPorts(CommunicationPorts ports, NodeDetails node) {
      node.masterHttpPort = ports.masterHttpPort;
      node.masterRpcPort = ports.masterRpcPort;
      node.tserverHttpPort = ports.tserverHttpPort;
      node.tserverRpcPort = ports.tserverRpcPort;
      node.redisServerHttpPort = ports.redisServerHttpPort;
      node.redisServerRpcPort = ports.redisServerRpcPort;
      node.yqlServerHttpPort = ports.yqlServerHttpPort;
      node.yqlServerRpcPort = ports.yqlServerRpcPort;
      node.ysqlServerHttpPort = ports.ysqlServerHttpPort;
      node.ysqlServerRpcPort = ports.ysqlServerRpcPort;
      node.nodeExporterPort = ports.nodeExporterPort;
    }
  }

  public static class ExtraDependencies {
    // Flag to install node_exporter on nodes.
    public boolean installNodeExporter = true;
  }

  // The primary device info.
  public DeviceInfo deviceInfo;

  // The universe against which this operation is being executed.
  public UUID universeUUID;

  // Expected version of the universe for operation execution. Set to -1 if an operation should
  // not verify expected version of the universe.
  public int expectedUniverseVersion;

  // If an AWS backed universe has chosen EBS volume encryption, this will be set to the
  // Amazon Resource Name (ARN) of the CMK to be used to generate data keys for volume encryption
  public String cmkArn;

  // Store encryption key provider specific configuration/authorization values
  public EncryptionAtRestConfig encryptionAtRestConfig = new EncryptionAtRestConfig();

  // The set of nodes that are part of this universe. Should contain nodes in both primary and
  // readOnly clusters.
  public Set<NodeDetails> nodeDetailsSet = null;

  // A list of ports to configure different parts of YB to listen on.
  public CommunicationPorts communicationPorts = new CommunicationPorts();

  // Dependencies that can be install on nodes or not
  public ExtraDependencies extraDependencies = new ExtraDependencies();
}
