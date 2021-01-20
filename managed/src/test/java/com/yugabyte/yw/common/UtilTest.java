package com.yugabyte.yw.common;

import com.google.common.collect.ImmutableMap;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.ClusterType;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.UserIntent;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.NodeDetails;
import com.yugabyte.yw.models.helpers.NodeDetails.NodeState;

import junitparams.JUnitParamsRunner;
import junitparams.Parameters;
import junitparams.converters.Nullable;
import junitparams.naming.TestCaseName;

import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.UUID;

import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.junit.MockitoJUnit;
import org.mockito.junit.MockitoRule;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.doReturn;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;

@RunWith(JUnitParamsRunner.class)
public class UtilTest extends FakeDBApplication {

  @Rule
  public MockitoRule mockitoRule = MockitoJUnit.rule();

  @Test
    public void testGetNodePrefixWithInvalidCustomerId() {
        try {
            Util.getNodePrefix(1L, "demo");
        } catch (Exception e) {
            assertEquals("Invalid Customer Id: 1", e.getMessage());
        }
    }

    @Test
    public void testGetNodePrefix() {
        Customer c = ModelFactory.testCustomer();
        String nodePrefix = Util.getNodePrefix(c.getCustomerId(), "demo");
        assertEquals("yb-tc-demo", nodePrefix);
    }

    @Test
    public void testGetKeysNotPresent() {
      Map<String, String> existing = ImmutableMap.of("Cust", "Test",
                                                     "Dept", "HR",
                                                     "Remove", "This",
                                                     "This", "Also");
      Map<String, String> newMap = ImmutableMap.of("Cust", "Test",
                                                   "Dept", "NewHR");
      assertEquals(Util.getKeysNotPresent(existing, newMap), "This,Remove");

      newMap = ImmutableMap.of("Cust", "Test",
                               "Dept", "NewHR",
                               "Remove", "ABCD",
                               "This", "BCDF",
                               "Just", "Coz");
      assertEquals(Util.getKeysNotPresent(existing, newMap), "");

      assertEquals(existing.size(), 4);
      assertEquals(newMap.size(), 5);
    }

    private static NodeDetails createNode(UUID uuid, boolean isMaster, NodeState state) {
      NodeDetails node = new NodeDetails();
      node.azUuid = uuid;
      node.isMaster = isMaster;
      node.state = state;
      return node;
    }

    @Test
    @Parameters(method = "parametersToTestNeedMasterQuorumRestore")
    @TestCaseName("{method} TC({0}) Zones count {0} nodes per zones {1}")
    public void testNeedMasterQuorumRestore(String tcName, int azCount, int[] countsInAZ,
        int[] mastersInAZ, int[] stoppedInAZ, int mastersToBeAdded, boolean[] expectedResults) {

      UUID[] azUUIDs = new UUID[azCount];
      for (int i = 0; i < azCount; i++) {
        azUUIDs[i] = UUID.randomUUID();
      }
      Cluster cluster = new Cluster(ClusterType.PRIMARY, null);
      Set<NodeDetails> nodeDetailsSet = prepareNodes(cluster, azUUIDs, azCount, countsInAZ,
          mastersInAZ, stoppedInAZ);

      // Going through all the zones trying to add a node
      NodeDetails currentNode = createNode(null, false, NodeState.Stopped);
      for (int i = 0; i < azCount; i++) {
        currentNode.azUuid = azUUIDs[i];
        assertEquals("Zone to probe " + i, expectedResults[i],
            Util.needMasterQuorumRestore(currentNode, nodeDetailsSet, mastersToBeAdded));
      }
    }

    private static Set<NodeDetails> prepareNodes(Cluster cluster, UUID[] azUUIDs, int azCount,
        int[] countsInAZ, int[] mastersInAZ, int[] stoppedInAZ) {
      Set<NodeDetails> nodeDetailsSet = new HashSet<>();
      for (int i = 0; i < azCount; i++) {
        int mastersToPlace = mastersInAZ[i];
        int stoppedCount = stoppedInAZ[i];
        for (int j = 0; j < countsInAZ[i]; j++) {
          boolean isStopped = (mastersToPlace <= 0) && (stoppedCount > 0);
          if (isStopped) {
            stoppedCount--;
          }
          NodeDetails node = createNode(azUUIDs[i], mastersToPlace > 0,
              isStopped ? NodeState.Stopped : NodeState.Live);
          node.placementUuid = cluster.uuid;
          nodeDetailsSet.add(node);
          mastersToPlace--;
        }
      }
      return nodeDetailsSet;
    }

    @SuppressWarnings("unused")
    private Object[] parametersToTestNeedMasterQuorumRestore() {
      return new Object[][] {
          // One zone without masters, one master to place
          { "A", 1, $(1), $(0), $(1), 1, $(true) },
          // Three zones, one stopped node in each zone
          // TODO: placement into z2 should not be allowed because masters should
          // be placed evenly; this will be fixed later
          { "B", 3, $(2, 3, 2), $(1, 2, 1), $(1, 1, 1), 1, $(true, true, true) },
          // Three zones, no masters in z1; should not allow to place master in z2 and z3
          { "C", 3, $(1, 1, 1), $(0, 1, 1), $(1, 0, 0), 1, $(true, false, false) },
      };
    }

    @Test
    @Parameters(method = "parametersToTestAreMastersUnderReplicated")
    @TestCaseName("{method} TC({0}) Zones count {1} RF {2} nodes per zones {3}")
    public void testAreMastersUnderReplicated(String tcName, int azCount, int replicationFactor,
        int[] countsInAZ, int[] mastersInAZ, int[] stoppedInAZ, boolean[] expectedResults) {

      UUID[] azUUIDs = new UUID[azCount];
      for (int i = 0; i < azCount; i++) {
        azUUIDs[i] = UUID.randomUUID();
      }

      UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
      Cluster cluster = new Cluster(ClusterType.PRIMARY, new UserIntent());
      cluster.uuid = UUID.randomUUID();
      cluster.userIntent.replicationFactor = replicationFactor;
      taskParams.clusters.add(cluster);
      taskParams.nodeDetailsSet = prepareNodes(cluster, azUUIDs, azCount, countsInAZ, mastersInAZ,
          stoppedInAZ);

      Universe universe = mock(Universe.class);
      doReturn(taskParams.nodeDetailsSet).when(universe).getNodes();
      doReturn(taskParams).when(universe).getUniverseDetails();
      doReturn(cluster).when(universe).getCluster(cluster.uuid);

      // Going through all the zones trying to add a node
      NodeDetails currentNode = createNode(null, false, NodeState.Stopped);
      currentNode.placementUuid = cluster.uuid;
      for (int i = 0; i < azCount; i++) {
        currentNode.azUuid = azUUIDs[i];
        assertEquals("Zone to probe " + i, expectedResults[i],
            Util.areMastersUnderReplicated(currentNode, universe));
      }
    }

    @SuppressWarnings("unused")
    private Object[] parametersToTestAreMastersUnderReplicated() {
      return new Object[][] {
          // AZ=1, RF=1, no running masters, test node in zone 0 => true
          { "A", 1, 1, $(1), $(0), $(1), $(true) },
          // AZ=1, RF=1, one master is running, test node in zone 0 => false
          { "B", 1, 1, $(1), $(1), $(1), $(false) },

          // AZ=3, RF=3, one stopped node in each zone, one master in each zone
          { "C", 3, 3, $(2, 2, 2), $(1, 1, 1), $(1, 1, 1), $(false, false, false) },
          // AZ=3, RF=3, one stopped node in each zone, no masters in z2
          { "D", 3, 3, $(2, 1, 2), $(1, 0, 1), $(1, 1, 1), $(false, true, false) },
          // AZ=3, RF=3, no masters in z1; should not allow to place master in z2 and z3
          { "E", 3, 3, $(1, 1, 1), $(0, 1, 1), $(1, 0, 0), $(true, false, false) },
          // AZ=3, RF=3, one stopped node in each zone, no masters at all
          { "F", 3, 3, $(1, 1, 1), $(0, 0, 0), $(1, 1, 1), $(true, true, true) },

          // AZ=3, RF=5, one stopped node in each zone
          // TODO: placement into second zone should not be allowed because masters should
          // be placed evenly; this will be fixed later
          { "G", 3, 5, $(2, 3, 2), $(1, 2, 1), $(1, 1, 1), $(true, true, true) },
          // AZ=3, RF=5, one stopped node in each zone, all masters are already placed
          { "H", 3, 5, $(2, 2, 2), $(2, 2, 1), $(1, 1, 1), $(false, false, false) },
          // AZ=3, RF=5, the same scenario, no stopped nodes
          { "I", 3, 5, $(2, 2, 2), $(2, 2, 1), $(0, 0, 0), $(false, false, false) },
      };
    }

    @SafeVarargs
    public static int[] $(int... values) {
      return values;
    }

    @SafeVarargs
    public static boolean[] $(boolean... values) {
      return values;
    }

    @Test
    public void testAreMastersUnderReplicatedForReadReplicaNode() {
      int azCount = 3;
      UUID[] azUUIDs = new UUID[azCount];
      for (int i = 0; i < azCount; i++) {
        azUUIDs[i] = UUID.randomUUID();
      }

      UniverseDefinitionTaskParams taskParams = new UniverseDefinitionTaskParams();
      Cluster cluster = new Cluster(ClusterType.PRIMARY, new UserIntent());
      cluster.uuid = UUID.randomUUID();
      cluster.userIntent.replicationFactor = 3;
      taskParams.clusters.add(cluster);
      taskParams.nodeDetailsSet = prepareNodes(cluster, azUUIDs, azCount, $(3, 3, 3), $(0, 0, 0),
          $(1, 1, 1));

      Cluster replicaCluster = new Cluster(ClusterType.ASYNC, new UserIntent());
      replicaCluster.uuid = UUID.randomUUID();
      taskParams.clusters.add(replicaCluster);

      Universe universe = mock(Universe.class);
      doReturn(taskParams.nodeDetailsSet).when(universe).getNodes();
      doReturn(taskParams).when(universe).getUniverseDetails();
      doReturn(cluster).when(universe).getCluster(cluster.uuid);
      doReturn(replicaCluster).when(universe).getCluster(replicaCluster.uuid);

      NodeDetails currentNode = createNode(null, false, NodeState.Stopped);
      currentNode.placementUuid = replicaCluster.uuid;
      taskParams.nodeDetailsSet.add(currentNode);

      assertFalse(Util.areMastersUnderReplicated(currentNode, universe));
    }

    @Test
    // @formatter:off
    @Parameters({ "filename, filename", // no path
                  ",",                  // empty name
                  "null, null",         // null values
                  "/test, test",
                  "/part1/part2/filename, filename",
                  "/part1/part2/," })
    // @formatter:on
    public void testGetFileName(@Nullable String fullName, @Nullable String fileName) {
      assertEquals(fileName, Util.getFileName(fullName));
    }

    // TODO: Add tests for other functions
}
