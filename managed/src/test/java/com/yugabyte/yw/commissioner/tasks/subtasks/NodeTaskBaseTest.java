// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.models.Customer;
import org.junit.Before;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.test.Helpers;
import play.test.WithApplication;

import org.pac4j.play.CallbackController;
import org.pac4j.play.store.PlayCacheSessionStore;
import org.pac4j.play.store.PlaySessionStore;

import java.util.Map;

import static org.mockito.Mockito.mock;
import static play.inject.Bindings.bind;

public class NodeTaskBaseTest extends WithApplication {
  NodeManager mockNodeManager;
  Customer defaultCustomer;
  Commissioner mockCommissioner;
  protected CallbackController mockCallbackController;
  protected PlayCacheSessionStore mockSessionStore;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
  }

  @Override
  protected Application provideApplication() {
    mockNodeManager = mock(NodeManager.class);
    mockCommissioner = mock(Commissioner.class);
    mockCallbackController = mock(CallbackController.class);
    mockSessionStore = mock(PlayCacheSessionStore.class);

    return new GuiceApplicationBuilder()
        .configure((Map) Helpers.inMemoryDatabase())
        .overrides(bind(NodeManager.class).toInstance(mockNodeManager))
        .overrides(bind(Commissioner.class).toInstance(mockCommissioner))
        .overrides(bind(CallbackController.class).toInstance(mockCallbackController))
        .overrides(bind(PlaySessionStore.class).toInstance(mockSessionStore))
        .build();
  }
}
