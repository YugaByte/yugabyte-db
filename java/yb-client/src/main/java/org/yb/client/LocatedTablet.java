/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * The following only applies to changes made to this file as part of YugaByte development.
 *
 * Portions Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 */
package org.yb.client;

import java.util.List;

import com.google.common.collect.Lists;
import com.google.common.collect.ImmutableList;
import org.yb.annotations.InterfaceAudience;
import org.yb.annotations.InterfaceStability;
import org.yb.consensus.Metadata.RaftPeerPB.Role;
import org.yb.master.Master.TabletLocationsPB;
import org.yb.master.Master.TabletLocationsPB.ReplicaPB;
import org.yb.Common.HostPortPB;

/**
 * Information about the locations of tablets in a YB table.
 * This should be treated as immutable data (it does not reflect
 * any updates the client may have heard since being constructed).
 */
@InterfaceAudience.Public
@InterfaceStability.Evolving
public class LocatedTablet {
  private final TabletLocationsPB pb;
  private final Partition partition;
  private final byte[] tabletId;

  private final List<Replica> replicas;

  LocatedTablet(TabletLocationsPB pb) {
    this.pb = pb;
    this.partition = ProtobufHelper.pbToPartition(pb.getPartition());
    this.tabletId = pb.getTabletId().toByteArray();

    List<Replica> reps = Lists.newArrayList();
    for (ReplicaPB repPb : pb.getReplicasList()) {
      reps.add(new Replica(repPb));
    }
    this.replicas = ImmutableList.copyOf(reps);
  }

  public List<Replica> getReplicas() {
    return replicas;
  }

  public Partition getPartition() {
    return partition;
  }

  /**
   * DEPRECATED: use {@link #getPartition()}
   */
  @Deprecated
  public byte[] getStartKey() {
    return getPartition().getPartitionKeyStart();
  }

  /**
   * DEPRECATED: use {@link #getPartition()}
   */
  @Deprecated()
  public byte[] getEndKey() {
    return getPartition().getPartitionKeyEnd();
  }

  public byte[] getTabletId() {
    return tabletId;
  }

  /**
   * Return the current leader, or null if there is none.
   */
  public Replica getLeaderReplica() {
    return getOneOfRoleOrNull(Role.LEADER);
  }

  /**
   * Return the first occurrence for the given role, or null if there is none.
   */
  private Replica getOneOfRoleOrNull(Role role) {
    for (Replica r : replicas) {
      if (r.getRole() == role.toString()) return r;
    }
    return null;
  }

  @Override
  public String toString() {
    return Bytes.pretty(tabletId) + " " + partition.toString();
  }

  public String toDebugString() {
    return pb.toString();
  }

  /**
   * One of the replicas of the tablet.
   */
  @InterfaceAudience.Public
  @InterfaceStability.Evolving
  public static class Replica {
    private final ReplicaPB pb;

    private Replica(ReplicaPB pb) {
      this.pb = pb;
    }

    public HostPortPB getRpcHostPort() {
      if (!pb.getTsInfo().getBroadcastAddressesList().isEmpty()) {
        return pb.getTsInfo().getBroadcastAddresses(0);
      }

      if (pb.getTsInfo().getPrivateRpcAddressesList().isEmpty()) {
        return null;
      }
      return pb.getTsInfo().getPrivateRpcAddressesList().get(0);
    }

    public String getRpcHost() {
      HostPortPB host_port = getRpcHostPort();
      return host_port == null ? null : host_port.getHost();
    }

    public Integer getRpcPort() {
      HostPortPB host_port = getRpcHostPort();
      return host_port == null ? null : host_port.getPort();
    }

    public String getRole() {
      return pb.getRole().toString();
    }

    public String toString() {
      return pb.toString();
    }

    public String getMemberType() {
      return pb.getMemberType().toString();
    }

    public String getTsUuid() {
      return pb.getTsInfo().getPermanentUuid().toStringUtf8();
    }

    public String getTsPlacementUuid() {
      return pb.getTsInfo().getPlacementUuid().toStringUtf8();
    }

    public org.yb.Common.CloudInfoPB getCloudInfo() {
      return pb.getTsInfo().getCloudInfo();
    }
  }

};
