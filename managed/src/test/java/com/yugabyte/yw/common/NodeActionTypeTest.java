// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.yugabyte.yw.models.CustomerTask;
import org.junit.Test;

import static com.yugabyte.yw.models.helpers.TaskType.DeleteNodeFromUniverse;
import static com.yugabyte.yw.models.helpers.TaskType.StartNodeInUniverse;
import static com.yugabyte.yw.models.helpers.TaskType.StopNodeInUniverse;
import static com.yugabyte.yw.models.helpers.TaskType.AddNodeToUniverse;
import static com.yugabyte.yw.models.helpers.TaskType.RemoveNodeFromUniverse;
import static com.yugabyte.yw.models.helpers.TaskType.ReleaseInstanceFromUniverse;
import static com.yugabyte.yw.models.helpers.TaskType.StartMasterOnNode;
import static org.junit.Assert.assertEquals;

public class NodeActionTypeTest {
  @Test
  public void testToString() {
    assertEquals("Starting", NodeActionType.START.toString(false));
    assertEquals("Started", NodeActionType.START.toString(true));
    assertEquals("Stopping", NodeActionType.STOP.toString(false));
    assertEquals("Stopped", NodeActionType.STOP.toString(true));
    assertEquals("Deleting", NodeActionType.DELETE.toString(false));
    assertEquals("Deleted", NodeActionType.DELETE.toString(true));
    assertEquals("Adding", NodeActionType.ADD.toString(false));
    assertEquals("Added", NodeActionType.ADD.toString(true));
    assertEquals("Removing", NodeActionType.REMOVE.toString(false));
    assertEquals("Removed", NodeActionType.REMOVE.toString(true));
    assertEquals("Releasing", NodeActionType.RELEASE.toString(false));
    assertEquals("Released", NodeActionType.RELEASE.toString(true));
    assertEquals("Starting Master", NodeActionType.START_MASTER.toString(false));
    assertEquals("Started Master", NodeActionType.START_MASTER.toString(true));
  }

  @Test
  public void testGetCommissionerTask() {
    assertEquals(StartNodeInUniverse, NodeActionType.START.getCommissionerTask());
    assertEquals(StopNodeInUniverse, NodeActionType.STOP.getCommissionerTask());
    assertEquals(DeleteNodeFromUniverse, NodeActionType.DELETE.getCommissionerTask());
    assertEquals(AddNodeToUniverse, NodeActionType.ADD.getCommissionerTask());
    assertEquals(RemoveNodeFromUniverse, NodeActionType.REMOVE.getCommissionerTask());
    assertEquals(ReleaseInstanceFromUniverse, NodeActionType.RELEASE.getCommissionerTask());
    assertEquals(StartMasterOnNode, NodeActionType.START_MASTER.getCommissionerTask());
  }

  @Test
  public void testGetCustomerTask() {
    assertEquals(CustomerTask.TaskType.Start, NodeActionType.START.getCustomerTask());
    assertEquals(CustomerTask.TaskType.Stop, NodeActionType.STOP.getCustomerTask());
    assertEquals(CustomerTask.TaskType.Delete, NodeActionType.DELETE.getCustomerTask());
    assertEquals(CustomerTask.TaskType.Add, NodeActionType.ADD.getCustomerTask());
    assertEquals(CustomerTask.TaskType.Release, NodeActionType.RELEASE.getCustomerTask());
    assertEquals(CustomerTask.TaskType.Remove, NodeActionType.REMOVE.getCustomerTask());
    assertEquals(CustomerTask.TaskType.StartMaster, NodeActionType.START_MASTER.getCustomerTask());
  }
}
