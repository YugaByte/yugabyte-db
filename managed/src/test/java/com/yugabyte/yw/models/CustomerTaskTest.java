// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.models.helpers.TaskType;
import org.junit.Before;
import org.junit.Test;
import play.libs.Json;

import java.time.Duration;
import java.time.Instant;
import java.time.temporal.ChronoUnit;
import java.util.*;
import java.util.stream.Collectors;

import static com.yugabyte.yw.models.CustomerTask.TaskType.Create;
import static org.hamcrest.CoreMatchers.*;
import static org.hamcrest.MatcherAssert.assertThat;
import static org.junit.Assert.*;

public class CustomerTaskTest extends FakeDBApplication {
  private Customer defaultCustomer;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
  }

  private static List<CustomerTask> deleteStaleTasks(Customer defaultCustomer, int days) {
    List<CustomerTask> staleTasks =
      CustomerTask.findOlderThan(defaultCustomer, Duration.ofDays(days));
    return staleTasks.stream()
      .filter(customerTask -> customerTask.cascadeDeleteCompleted() > 0)
      .collect(Collectors.toList());
  }

  private CustomerTask createTask(CustomerTask.TargetType targetType,
                                  UUID targetUUID, CustomerTask.TaskType taskType) {
    UUID taskUUID = UUID.randomUUID();
    return CustomerTask.create(defaultCustomer, targetUUID, taskUUID,
      targetType, taskType, "Foo");
  }

  private CustomerTask createTaskTree(CustomerTask.TargetType targetType, UUID targetUUID,
                                      CustomerTask.TaskType taskType) {
    return createTaskTree(targetType, targetUUID, taskType, 3,
      Optional.of(TaskInfo.State.Success), true);
  }

  private CustomerTask createTaskTree(CustomerTask.TargetType targetType, UUID targetUUID,
                                      CustomerTask.TaskType taskType, int depth,
                                      Optional<TaskInfo.State> completeRoot,
                                      boolean completeSubtasks) {
    UUID rootTaskUUID = null;
    if (depth > 1) {
      TaskInfo rootTaskInfo = buildTaskInfo(null, TaskType.CreateUniverse);
      rootTaskUUID = rootTaskInfo.getTaskUUID();
      completeRoot.ifPresent(rootTaskInfo::setTaskState);
      rootTaskInfo.save();
    }
    if (depth > 2) {
      TaskInfo subtask0 = buildTaskInfo(rootTaskUUID, TaskType.AnsibleSetupServer);
      if (completeSubtasks) {
        subtask0.setTaskState(TaskInfo.State.Failure);
      }
      subtask0.save();
      TaskInfo subtask1 = buildTaskInfo(rootTaskUUID, TaskType.AnsibleConfigureServers);
      if (completeSubtasks) {
        subtask1.setTaskState(TaskInfo.State.Success);
      }
      subtask1.save();
    }
    return CustomerTask.create(defaultCustomer, targetUUID, rootTaskUUID,
      targetType, taskType, "Foo");
  }

  private TaskInfo buildTaskInfo(UUID parentUUID, TaskType taskType) {
    TaskInfo taskInfo;
    taskInfo = new TaskInfo(taskType);
    UUID taskUUID = UUID.randomUUID();
    taskInfo.setTaskUUID(taskUUID);
    taskInfo.setTaskDetails(Json.newObject());
    taskInfo.setOwner("");
    if (parentUUID != null) {
      taskInfo.setParentUuid(parentUUID);
    }
    return taskInfo;
  }

  @Test
  public void testCreateInstance() {
    long expectedId = 1;
    for (CustomerTask.TargetType targetType : CustomerTask.TargetType.values()) {
      UUID targetUUID = UUID.randomUUID();
      CustomerTask th = createTask(targetType, targetUUID, Create);
      Date currentDate = new Date();
      assertSame(expectedId++, th.getId());
      assertTrue(currentDate.compareTo(th.getCreateTime()) >= 0);
      assertThat(th.getFriendlyDescription(), is(allOf(notNullValue(),
        equalTo("Creating " + targetType.toString() + " : Foo"))));
      assertThat(th.getTargetUUID(), is(equalTo(targetUUID)));
      assertThat(th.getCustomerUUID(), is(equalTo(defaultCustomer.uuid)));
    }
  }

  @Test
  public void testMarkTaskComplete() {
    for (CustomerTask.TargetType targetType : CustomerTask.TargetType.values()) {
      UUID targetUUID = UUID.randomUUID();
      CustomerTask th = createTask(targetType, targetUUID, Create);
      assertEquals(th.getTarget(), targetType);
      assertThat(th.getFriendlyDescription(), is(allOf(notNullValue(),
        equalTo("Creating " + targetType.toString() + " : Foo"))));
      th.markAsCompleted();
      assertThat(th.getFriendlyDescription(), is(allOf(notNullValue(),
        equalTo("Created " + targetType.toString() + " : Foo"))));
      assertTrue(th.getCreateTime().compareTo(th.getCompletionTime()) <= 0);
      Date completionTime = th.getCompletionTime();
      // Calling mark as completed shouldn't change the time.
      th.markAsCompleted();
      assertEquals(completionTime, th.getCompletionTime());
    }
  }

  @Test
  public void testFriendlyDescriptions() {
    UUID targetUUID = UUID.randomUUID();
    for (CustomerTask.TargetType targetType : CustomerTask.TargetType.values()) {
      for (CustomerTask.TaskType taskType : CustomerTask.TaskType.filteredValues()) {
        CustomerTask th = createTask(targetType, targetUUID, taskType);
        assertThat(th.getFriendlyDescription(), is(allOf(notNullValue(),
          equalTo(taskType.toString(false) + targetType + " : Foo"))));
        th.markAsCompleted();
        assertThat(th.getFriendlyDescription(), is(allOf(notNullValue(),
          equalTo(taskType.toString(true) + targetType + " : Foo"))));
      }
    }
  }

  @Test(expected = NullPointerException.class)
  public void testCascadeDeleteCompleted_throwsIfIncomplete() {
    UUID targetUUID = UUID.randomUUID();
    CustomerTask th = createTask(CustomerTask.TargetType.Table, targetUUID, Create);
    // do not complete it and try cascadeDeleteCompleted
    th.cascadeDeleteCompleted();
  }

  @Test
  public void testCascadeDelete_noSubtasks_success() {
    UUID targetUUID = UUID.randomUUID();
    CustomerTask th = createTaskTree(CustomerTask.TargetType.Table, targetUUID, Create, 2,
      Optional.of(TaskInfo.State.Success),
      true);
    th.markAsCompleted();
    assertEquals(2, th.cascadeDeleteCompleted());
    assertNull(CustomerTask.findByTaskUUID(th.getTaskUUID()));
  }

  @Test
  public void testCascadeDelete_taskInfoIncomplete_skipped() {
    UUID targetUUID = UUID.randomUUID();
    CustomerTask th = createTaskTree(CustomerTask.TargetType.Table, targetUUID, Create, 3,
      Optional.empty(), true);
    th.markAsCompleted();
    assertEquals(0, th.cascadeDeleteCompleted());
    assertEquals(th, CustomerTask.findByTaskUUID(th.getTaskUUID()));
  }


  @Test
  public void testCascadeDeleteSuccessfulTask_subtasksIncomplete_skipped() {
    UUID targetUUID = UUID.randomUUID();
    CustomerTask th = createTaskTree(CustomerTask.TargetType.Table, targetUUID, Create, 3,
      Optional.of(TaskInfo.State.Success),
      false);
    th.markAsCompleted();
    assertEquals(0, th.cascadeDeleteCompleted());
    assertEquals(th, CustomerTask.findByTaskUUID(th.getTaskUUID()));
  }

  @Test
  public void testCascadeDeleteFailedTask_subtasksIncomplete_success() {
    UUID targetUUID = UUID.randomUUID();
    CustomerTask th = createTaskTree(CustomerTask.TargetType.Table, targetUUID, Create, 3,
      Optional.of(TaskInfo.State.Failure),
      false);
    th.markAsCompleted();
    assertEquals(4, th.cascadeDeleteCompleted());
    assertTrue(CustomerTask.find.all().isEmpty());
    assertTrue(TaskInfo.find.all().isEmpty());
  }

  @Test
  public void testDeleteStaleTasks_success() {
    Random rng = new Random();
    UUID targetUUID = UUID.randomUUID();
    Instant now = Instant.now();
    for (int i = 0; i < 3; i++) {
      CustomerTask th = createTaskTree(CustomerTask.TargetType.Table, targetUUID, Create);
      long completionTimestamp = now.minus(rng.nextInt(5), ChronoUnit.DAYS).toEpochMilli();
      th.markAsCompleted(new Date(completionTimestamp));
    }
    List<CustomerTask> staleTasks = deleteStaleTasks(defaultCustomer, 5);
    assertTrue(staleTasks.isEmpty());
    for (int i = 0; i < 4; i++) {
      CustomerTask th = createTaskTree(CustomerTask.TargetType.Universe, targetUUID, Create);
      long completionTimestamp = now.minus(5 + rng.nextInt(100), ChronoUnit.DAYS).toEpochMilli();
      th.markAsCompleted(new Date(completionTimestamp));
    }
    assertEquals(7, CustomerTask.find.all().size());
    assertEquals(21, TaskInfo.find.all().size());

    staleTasks = deleteStaleTasks(defaultCustomer, 5);
    assertEquals(4, staleTasks.size());
    for (int i = 0; i < 4; i++) {
      assertEquals(CustomerTask.TargetType.Universe, staleTasks.get(i).getTarget());
    }
    assertEquals(3, CustomerTask.find.all().size());
    assertEquals(9, TaskInfo.find.all().size());
    staleTasks = deleteStaleTasks(defaultCustomer, 0);
    assertEquals(3, staleTasks.size());
    for (int i = 0; i < 3; i++) {
      assertEquals(CustomerTask.TargetType.Table, staleTasks.get(i).getTarget());
    }
    assertTrue(CustomerTask.find.all().isEmpty());
    assertTrue(TaskInfo.find.all().isEmpty());
  }
}
