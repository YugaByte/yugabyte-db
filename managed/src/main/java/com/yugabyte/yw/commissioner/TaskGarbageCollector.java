package com.yugabyte.yw.commissioner;

import akka.actor.ActorSystem;
import akka.actor.Scheduler;
import com.google.common.annotations.VisibleForTesting;
import com.yugabyte.yw.common.config.RuntimeConfigFactory;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.CustomerTask;
import io.prometheus.client.CollectorRegistry;
import io.prometheus.client.Counter;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import scala.concurrent.ExecutionContext;

import javax.inject.Inject;
import javax.inject.Singleton;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.Callable;

@Singleton
public class TaskGarbageCollector {
  public static final Logger LOG = LoggerFactory.getLogger(TaskGarbageCollector.class);

  // TODO: Set this to true once we start collecting prometheus metric:
  @VisibleForTesting
  public static boolean EXPORT_PROM_METRIC = false;

  // Counter names
  static final String CUSTOMER_TASK_METRIC_NAME = "yw_customer_task_gc_count";
  static final String TASK_INFO_METRIC_NAME = "yw_task_info_gc_count";
  static final String NUM_TASK_GC_RUNS = "yw_task_gc_run_count";
  static final String NUM_TASK_GC_ERRORS = "yw_task_gc_error_count";

  // Counter label
  static final String CUSTOMER_UUID_LABEL = "customer_uuid";

  // Config names
  static final String YB_TASK_GC_GC_CHECK_INTERVAL = "yb.taskGC.gc_check_interval";
  static final String YB_TASK_GC_TASK_RETENTION_DURATION = "yb.taskGC.task_retention_duration";

  private final Scheduler scheduler;
  private final RuntimeConfigFactory runtimeConfigFactory;
  private final ExecutionContext executionContext;
  private final Optional<Counter> purgedCustomerTaskCount;
  private final Optional<Counter> purgedTaskInfoCount;
  private final Optional<Counter> numTaskGCRuns;
  private final Optional<Counter> numTaskGCErrors;

  private static <V> Optional<V> tryOrLog(Callable<V> callable, String failureMessage) {
    if (EXPORT_PROM_METRIC) {
      try {
        return Optional.of(callable.call());
      } catch (Exception exception) {
        LOG.warn(failureMessage + ": " + exception.toString());
      }
    }
    return Optional.empty();
  }

  private static Optional<Counter> registerNumTaskGCErrors(CollectorRegistry promRegistry) {
    return tryOrLog(
      () -> Counter.build(NUM_TASK_GC_ERRORS,
        "Number of failed customer_task delete attempts")
        .register(promRegistry),
      "Failed to build prometheus Counter for name: " + NUM_TASK_GC_ERRORS);
  }

  private static Optional<Counter> registerNumTaskGCRuns(CollectorRegistry promRegistry) {
    return tryOrLog(
      () -> Counter.build(NUM_TASK_GC_RUNS,
        "Number of times customer gc checks are run")
        .register(promRegistry),
      "Failed to build prometheus Counter for name: " + NUM_TASK_GC_RUNS);
  }

  private static Optional<Counter> registerPurgedTaskInfoCount(CollectorRegistry promRegistry) {
    return tryOrLog(
      () -> Counter.build(TASK_INFO_METRIC_NAME,
        "Number of tasks info rows purged for a customer")
        .labelNames(CUSTOMER_UUID_LABEL)
        .register(promRegistry),
      "Failed to build prometheus Counter for name: " + TASK_INFO_METRIC_NAME);
  }

  private static Optional<Counter> registerPurgedCustomerTaskCount(CollectorRegistry promRegistry) {
    return tryOrLog(
      () -> Counter.build(CUSTOMER_TASK_METRIC_NAME,
        "Number of old completed customer tasks purged for a customer")
        .labelNames(CUSTOMER_UUID_LABEL)
        .register(promRegistry),
      "Failed to build prometheus Counter for name: " + CUSTOMER_TASK_METRIC_NAME);
  }

  @Inject
  public TaskGarbageCollector(
    ActorSystem actorSystem,
    RuntimeConfigFactory runtimeConfigFactory,
    ExecutionContext executionContext) {
    this(actorSystem.scheduler(), runtimeConfigFactory, executionContext,
      CollectorRegistry.defaultRegistry);
  }

  @VisibleForTesting
  TaskGarbageCollector(
    Scheduler scheduler,
    RuntimeConfigFactory runtimeConfigFactory,
    ExecutionContext executionContext,
    CollectorRegistry promRegistry) {
    this.scheduler = scheduler;
    this.runtimeConfigFactory = runtimeConfigFactory;
    this.executionContext = executionContext;

    // Register metric
    purgedCustomerTaskCount = registerPurgedCustomerTaskCount(promRegistry);
    purgedTaskInfoCount = registerPurgedTaskInfoCount(promRegistry);
    numTaskGCRuns = registerNumTaskGCRuns(promRegistry);
    numTaskGCErrors = registerNumTaskGCErrors(promRegistry);
  }

  public void start() {
    Duration gcInterval = this.gcCheckInterval();
    if (gcInterval.isZero()) {
      LOG.info("yb.taskGC.gc_check_interval set to 0.");
      LOG.warn("!!! TASK GC DISABLED !!!");
    } else {
      LOG.info("Scheduling TaskGC every " + gcInterval);
      scheduler.schedule(
        Duration.ZERO, // InitialDelay
        gcInterval,
        this::scheduleRunner,
        this.executionContext
      );
    }
  }

  private void scheduleRunner() {
    Customer.getAll().forEach(this::checkCustomer);
  }

  private void checkCustomer(Customer c) {
    List<CustomerTask> staleTasks = CustomerTask.findOlderThan(c, taskRetentionDuration(c));
    purgeStaleTasks(c, staleTasks);
  }

  @VisibleForTesting
  void purgeStaleTasks(Customer c, List<CustomerTask> staleTasks) {
    numTaskGCRuns.ifPresent(Counter::inc);
    int numRowsGCdInThisRun = 0;
    for (CustomerTask customerTask : staleTasks) {
      int numRowsDeleted = customerTask.cascadeDeleteCompleted();
      numRowsGCdInThisRun += numRowsDeleted;
      if (numRowsDeleted > 0) {
        purgedCustomerTaskCount
          .ifPresent(counter -> counter.labels(c.getUuid().toString()).inc());
        purgedTaskInfoCount
          .ifPresent(counter -> counter.labels(c.getUuid().toString()).inc(numRowsDeleted - 1));
      } else {
        numTaskGCErrors.ifPresent(Counter::inc);
      }
    }
    LOG.info("Garbage collected {} rows", numRowsGCdInThisRun);
  }

  /**
   * The interval at which the gc checker will run.
   */
  private Duration gcCheckInterval() {
    return runtimeConfigFactory.staticApplicationConf()
      .getDuration(YB_TASK_GC_GC_CHECK_INTERVAL);
  }

  /**
   * For how many days to retain a completed task before garbage collecting it.
   */
  private Duration taskRetentionDuration(Customer customer) {
    return runtimeConfigFactory.forCustomer(customer)
      .getDuration(YB_TASK_GC_TASK_RETENTION_DURATION);
  }
}
