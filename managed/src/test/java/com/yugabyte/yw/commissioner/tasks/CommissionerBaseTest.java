// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.commissioner.tasks;

import com.google.common.net.HostAndPort;

import com.yugabyte.yw.cloud.AWSInitializer;
import com.yugabyte.yw.cloud.GCPInitializer;
import com.yugabyte.yw.commissioner.CallHome;
import com.yugabyte.yw.commissioner.HealthChecker;
import com.yugabyte.yw.commissioner.QueryAlerts;
import com.yugabyte.yw.commissioner.SubTaskGroupQueue;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.common.AccessManager;
import com.yugabyte.yw.common.CloudQueryHelper;
import com.yugabyte.yw.common.ConfigHelper;
import com.yugabyte.yw.common.DnsManager;
import com.yugabyte.yw.common.KubernetesManager;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.NetworkManager;
import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.common.SwamperHelper;
import com.yugabyte.yw.common.TableManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.TaskInfo;
import org.junit.Before;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.test.Helpers;
import play.test.WithApplication;

import org.pac4j.play.CallbackController;
import org.pac4j.play.store.PlayCacheSessionStore;
import org.pac4j.play.store.PlaySessionStore;

import org.yb.client.AbstractModifyMasterClusterConfig;
import org.yb.client.ChangeMasterClusterConfigResponse;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.client.GetLoadMovePercentResponse;
import org.yb.client.ListTabletServersResponse;
import org.yb.client.IsServerReadyResponse;
import org.yb.client.YBClient;
import org.yb.master.Master;

import java.util.Map;
import java.util.UUID;

import static org.mockito.Mockito.mock;
import static org.mockito.Matchers.any;
import static org.mockito.Matchers.anyInt;
import static org.mockito.Matchers.anyBoolean;
import static org.mockito.Matchers.anyLong;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import static play.inject.Bindings.bind;

public abstract class CommissionerBaseTest extends WithApplication {
  private int maxRetryCount = 200;
  protected AccessManager mockAccessManager;
  protected NetworkManager mockNetworkManager;
  protected ConfigHelper mockConfigHelper;
  protected AWSInitializer mockAWSInitializer;
  protected GCPInitializer mockGCPInitializer;
  protected YBClientService mockYBClient;
  protected NodeManager mockNodeManager;
  protected DnsManager mockDnsManager;
  protected TableManager mockTableManager;
  protected CloudQueryHelper mockCloudQueryHelper;
  protected KubernetesManager mockKubernetesManager;
  protected SwamperHelper mockSwamperHelper;
  protected CallHome mockCallHome;
  protected HealthChecker mockHealthChecker;
  protected CallbackController mockCallbackController;
  protected PlayCacheSessionStore mockSessionStore;
  protected ApiHelper mockApiHelper;
  protected QueryAlerts mockQueryAlerts;

  Customer defaultCustomer;
  Provider defaultProvider;
  Provider gcpProvider;

  @Before
  public void setUp() {
    defaultCustomer = ModelFactory.testCustomer();
    defaultProvider = ModelFactory.awsProvider(defaultCustomer);
    gcpProvider = ModelFactory.gcpProvider(defaultCustomer);
  }

  @Override
  protected Application provideApplication() {
    mockAccessManager = mock(AccessManager.class);
    mockNetworkManager = mock(NetworkManager.class);
    mockConfigHelper = mock(ConfigHelper.class);
    mockAWSInitializer = mock(AWSInitializer.class);
    mockGCPInitializer = mock(GCPInitializer.class);
    mockYBClient = mock(YBClientService.class);
    mockNodeManager = mock(NodeManager.class);
    mockDnsManager = mock(DnsManager.class);
    mockCloudQueryHelper = mock(CloudQueryHelper.class);
    mockTableManager = mock(TableManager.class);
    mockKubernetesManager = mock(KubernetesManager.class);
    mockSwamperHelper = mock(SwamperHelper.class);
    mockCallHome = mock(CallHome.class);
    mockHealthChecker = mock(HealthChecker.class);
    mockCallbackController = mock(CallbackController.class);
    mockSessionStore = mock(PlayCacheSessionStore.class);
    mockApiHelper = mock(ApiHelper.class);
    mockQueryAlerts = mock(QueryAlerts.class);

    return new GuiceApplicationBuilder()
        .configure((Map) Helpers.inMemoryDatabase())
        .overrides(bind(AccessManager.class).toInstance(mockAccessManager))
        .overrides(bind(NetworkManager.class).toInstance(mockNetworkManager))
        .overrides(bind(ConfigHelper.class).toInstance(mockConfigHelper))
        .overrides(bind(AWSInitializer.class).toInstance(mockAWSInitializer))
        .overrides(bind(GCPInitializer.class).toInstance(mockGCPInitializer))
        .overrides(bind(YBClientService.class).toInstance(mockYBClient))
        .overrides(bind(NodeManager.class).toInstance(mockNodeManager))
        .overrides(bind(DnsManager.class).toInstance(mockDnsManager))
        .overrides(bind(CloudQueryHelper.class).toInstance(mockCloudQueryHelper))
        .overrides(bind(TableManager.class).toInstance(mockTableManager))
        .overrides(bind(KubernetesManager.class).toInstance(mockKubernetesManager))
        .overrides(bind(SwamperHelper.class).toInstance(mockSwamperHelper))
        .overrides(bind(HealthChecker.class).toInstance(mockHealthChecker))
        .overrides(bind(CallHome.class).toInstance(mockCallHome))
        .overrides(bind(CallbackController.class).toInstance(mockCallbackController))
        .overrides(bind(PlaySessionStore.class).toInstance(mockSessionStore))
        .overrides(bind(ApiHelper.class).toInstance(mockApiHelper))
        .overrides(bind(QueryAlerts.class).toInstance(mockQueryAlerts))
        .build();
  }

  public void mockWaits(YBClient mockClient) {
    mockWaits(mockClient, 1);
  }

  public void mockWaits(YBClient mockClient, int version) {
    IsServerReadyResponse okReadyResp = new IsServerReadyResponse(0, "", null, 0, 0);
    try {
      // PlacementUtil mock.
      Master.SysClusterConfigEntryPB.Builder configBuilder =
        Master.SysClusterConfigEntryPB.newBuilder().setVersion(version);
      GetMasterClusterConfigResponse gcr = new GetMasterClusterConfigResponse(0, "", configBuilder.build(), null);
      when(mockClient.getMasterClusterConfig()).thenReturn(gcr);
    } catch (Exception e) {
      e.printStackTrace();
    }
  }

  protected TaskInfo waitForTask(UUID taskUUID) throws InterruptedException {
    int numRetries = 0;
    while (numRetries < maxRetryCount) {
      TaskInfo taskInfo = TaskInfo.get(taskUUID);
      if (taskInfo.getTaskState() == TaskInfo.State.Success ||
          taskInfo.getTaskState() == TaskInfo.State.Failure) {
        return taskInfo;
      }
      Thread.sleep(1000);
      numRetries++;
    }
    throw new RuntimeException("WaitFor task exceeded maxRetries! Task state is " +
                               TaskInfo.get(taskUUID).getTaskState());
  }
}
