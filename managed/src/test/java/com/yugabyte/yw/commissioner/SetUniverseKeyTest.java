// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner;

import com.yugabyte.yw.commissioner.SetUniverseKey;
import com.yugabyte.yw.common.FakeDBApplication;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.models.Customer;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.Mock;
import org.mockito.Mockito;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.eq;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import org.mockito.runners.MockitoJUnitRunner;
import play.api.Play;

@RunWith(MockitoJUnitRunner.class)
public class SetUniverseKeyTest extends FakeDBApplication {
  Customer customer1;
  Customer customer2;

  @Before
  public void setUp() {
    customer1 = ModelFactory.testCustomer();
    customer2 = ModelFactory.testCustomer("tc2", "test2@customer.com");
  }

  @Test
  public void testCustomerError() {
    SetUniverseKey task = Play.current().injector().instanceOf(SetUniverseKey.class);
    Exception customerTaskException = new RuntimeException();
    Mockito.doCallRealMethod().when(task).setRunningState(any());
    Mockito.doThrow(customerTaskException).when(task).setCustomerUniverseKeys(any());
    Mockito.doCallRealMethod().when(task).scheduleRunner();
    task.setRunningState(new AtomicBoolean(false));
    task.scheduleRunner();
    // Ensure that the task runs for each customer even though they both error out
    verify(task, times(2)).setCustomerUniverseKeys(any());
    verify(task, times(1)).handleCustomerError(eq(customer1.uuid), eq(customerTaskException));
    verify(task, times(1)).handleCustomerError(eq(customer2.uuid), eq(customerTaskException));
    verify(task, times(0)).setUniverseKey(any());

  }
}
