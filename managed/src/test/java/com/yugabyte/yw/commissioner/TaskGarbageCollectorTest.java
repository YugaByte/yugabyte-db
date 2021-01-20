package com.yugabyte.yw.commissioner;

import akka.actor.ActorSystem;
import akka.actor.Scheduler;
import com.typesafe.config.Config;
import com.yugabyte.yw.common.config.RuntimeConfigFactory;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerTask;
import io.prometheus.client.CollectorRegistry;
import junit.framework.TestCase;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.junit.MockitoJUnitRunner;
import scala.concurrent.ExecutionContext;

import java.time.Duration;
import java.util.Collections;
import java.util.UUID;

import static com.yugabyte.yw.commissioner.TaskGarbageCollector.*;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.*;

@RunWith(MockitoJUnitRunner.class)
public class TaskGarbageCollectorTest extends TestCase {

  private void checkCounters(UUID customerUuid, Double expectedNumRuns, Double expectedErrors,
                             Double expectedCustomerTaskGC, Double expectedTaskInfoGC) {
    assertEquals(expectedNumRuns, testRegistry.getSampleValue(NUM_TASK_GC_RUNS));
    assertEquals(expectedErrors, testRegistry.getSampleValue(NUM_TASK_GC_ERRORS));
    assertEquals(expectedCustomerTaskGC,
      testRegistry.getSampleValue(
        CUSTOMER_TASK_METRIC_NAME,
        new String[]{CUSTOMER_UUID_LABEL},
        new String[]{customerUuid.toString()}));
    assertEquals(expectedTaskInfoGC,
      testRegistry.getSampleValue(
        TASK_INFO_METRIC_NAME,
        new String[]{CUSTOMER_UUID_LABEL},
        new String[]{customerUuid.toString()}));
  }

  @Mock
  ActorSystem mockActorSystem;

  @Mock
  Scheduler mockScheduler;

  @Mock
  Config mockAppConfig;

  @Mock
  RuntimeConfigFactory mockRuntimeConfigFactory;

  @Mock
  ExecutionContext mockExecutionContext;

  @Mock
  Customer mockCustomer;

  @Mock
  CustomerTask mockCustomerTask;

  CollectorRegistry testRegistry;

  @Before
  public void setUp() {
    EXPORT_PROM_METRIC = true;
    testRegistry = new CollectorRegistry();
    when(mockRuntimeConfigFactory.staticApplicationConf()).thenReturn(mockAppConfig);
  }

  @Test
  public void testIgnorePromError() {
    CollectorRegistry mockCollectorRegistry = mock(CollectorRegistry.class);
    doThrow(IllegalArgumentException.class).when(mockCollectorRegistry).register(any());
    new TaskGarbageCollector(mockScheduler, mockRuntimeConfigFactory, mockExecutionContext,
      mockCollectorRegistry);
  }

  @Test
  public void testDefaultProm() {
    when(mockActorSystem.scheduler()).thenReturn(mockScheduler);
    new TaskGarbageCollector(mockActorSystem, mockRuntimeConfigFactory, mockExecutionContext);
  }

  @Test
  public void testStart_disabled() {
    when(mockAppConfig.getDuration(YB_TASK_GC_GC_CHECK_INTERVAL))
      .thenReturn(Duration.ZERO);
    TaskGarbageCollector gc = new TaskGarbageCollector(mockScheduler, mockRuntimeConfigFactory,
      mockExecutionContext, testRegistry);
    gc.start();
    verifyZeroInteractions(mockScheduler);
  }

  @Test
  public void testStart_enabled() {
    when(mockAppConfig.getDuration(YB_TASK_GC_GC_CHECK_INTERVAL))
      .thenReturn(Duration.ofDays(1));
    TaskGarbageCollector gc = new TaskGarbageCollector(mockScheduler,
      mockRuntimeConfigFactory, mockExecutionContext, testRegistry);
    gc.start();
    verify(mockScheduler, times(1))
      .schedule(eq(Duration.ZERO),
        eq(Duration.ofDays(1)),
        any(),
        eq(mockExecutionContext));
  }


  @Test
  public void testPurge_noneStale() {
    UUID customerUuid = UUID.randomUUID();

    TaskGarbageCollector gc = new TaskGarbageCollector(
      mockScheduler, mockRuntimeConfigFactory, mockExecutionContext, testRegistry);
    gc.purgeStaleTasks(mockCustomer, Collections.emptyList());

    checkCounters(customerUuid, 1.0, 0.0, null, null);
  }

  @Test
  public void testPurge() {
    UUID customerUuid = UUID.randomUUID();
    when(mockCustomer.getUuid()).thenReturn(customerUuid);
    // Pretend we deleted 5 rows in all:
    when(mockCustomerTask.cascadeDeleteCompleted()).thenReturn(5);

    TaskGarbageCollector gc = new TaskGarbageCollector(
      mockScheduler, mockRuntimeConfigFactory, mockExecutionContext, testRegistry);
    gc.purgeStaleTasks(mockCustomer, Collections.singletonList(mockCustomerTask));

    checkCounters(customerUuid, 1.0, 0.0, 1.0, 4.0);
  }

  // Test that if we do not delete when there are referential integrity issues; then we report such
  // error in counter.
  @Test
  public void testPurge_invalidData() {
    UUID customerUuid = UUID.randomUUID();
    // Pretend we deleted 5 rows in all:
    when(mockCustomerTask.cascadeDeleteCompleted()).thenReturn(0);

    TaskGarbageCollector gc = new TaskGarbageCollector(
      mockScheduler, mockRuntimeConfigFactory, mockExecutionContext, testRegistry);
    gc.purgeStaleTasks(mockCustomer, Collections.singletonList(mockCustomerTask));

    checkCounters(customerUuid, 1.0, 1.0, null, null);
  }
}
