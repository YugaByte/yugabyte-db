// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.google.common.collect.Maps;
import com.yugabyte.yw.cloud.CloudAPI;
import com.yugabyte.yw.commissioner.*;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import org.pac4j.play.CallbackController;
import org.pac4j.play.store.PlayCacheSessionStore;
import org.pac4j.play.store.PlaySessionStore;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.test.Helpers;
import play.test.WithApplication;

import java.util.concurrent.Executors;

import static org.mockito.Mockito.mock;
import static play.inject.Bindings.bind;

public class FakeDBApplication extends WithApplication {
  public Commissioner mockCommissioner = mock(Commissioner.class);
  public CallHome mockCallHome = mock(CallHome.class);
  public ApiHelper mockApiHelper = mock(ApiHelper.class);
  public KubernetesManager mockKubernetesManager = mock(KubernetesManager.class);
  public HealthChecker mockHealthChecker = mock(HealthChecker.class);
  public EncryptionAtRestManager mockEARManager = mock(EncryptionAtRestManager.class);
  public SetUniverseKey mockSetUniverseKey = mock(SetUniverseKey.class);
  public CallbackController mockCallbackController = mock(CallbackController.class);
  public PlayCacheSessionStore mockSessionStore = mock(PlayCacheSessionStore.class);
  public AccessManager mockAccessManager = mock(AccessManager.class);
  public TemplateManager mockTemplateManager = mock(TemplateManager.class);
  public MetricQueryHelper mockMetricQueryHelper = mock(MetricQueryHelper.class);
  public CloudQueryHelper mockCloudQueryHelper = mock(CloudQueryHelper.class);
  public CloudAPI.Factory mockCloudAPIFactory = mock(CloudAPI.Factory.class);
  public ReleaseManager mockReleaseManager = mock(ReleaseManager.class);
  public YBClientService mockService = mock(YBClientService.class);
  public DnsManager mockDnsManager = mock(DnsManager.class);
  public NetworkManager mockNetworkManager = mock(NetworkManager.class);
  public YamlWrapper mockYamlWrapper = mock(YamlWrapper.class);
  public QueryAlerts mockQueryAlerts = mock(QueryAlerts.class);
  public Executors mockExecutors = mock(Executors.class);

  @Override
  protected Application provideApplication() {
    return new GuiceApplicationBuilder()
      .configure(Maps.newHashMap(Helpers.inMemoryDatabase()))
      .overrides(bind(ApiHelper.class).toInstance(mockApiHelper))
      .overrides(bind(Commissioner.class).toInstance(mockCommissioner))
      .overrides(bind(CallHome.class).toInstance(mockCallHome))
      .overrides(bind(HealthChecker.class).toInstance(mockHealthChecker))
      .overrides(bind(Executors.class).toInstance(mockExecutors))
      .overrides(bind(EncryptionAtRestManager.class).toInstance(mockEARManager))
      .overrides(bind(SetUniverseKey.class).toInstance(mockSetUniverseKey))
      .overrides(bind(KubernetesManager.class).toInstance(mockKubernetesManager))
      .overrides(bind(CallbackController.class).toInstance(mockCallbackController))
      .overrides(bind(PlaySessionStore.class).toInstance(mockSessionStore))
      .overrides(bind(AccessManager.class).toInstance(mockAccessManager))
      .overrides(bind(TemplateManager.class).toInstance(mockTemplateManager))
      .overrides(bind(MetricQueryHelper.class).toInstance(mockMetricQueryHelper))
      .overrides(bind(CloudQueryHelper.class).toInstance(mockCloudQueryHelper))
      .overrides(bind(ReleaseManager.class).toInstance(mockReleaseManager))
      .overrides(bind(YBClientService.class).toInstance(mockService))
      .overrides(bind(NetworkManager.class).toInstance(mockNetworkManager))
      .overrides(bind(DnsManager.class).toInstance(mockDnsManager))
      .overrides(bind(YamlWrapper.class).toInstance(mockYamlWrapper))
      .overrides(bind(QueryAlerts.class).toInstance(mockQueryAlerts))
      .overrides(bind(CloudAPI.Factory.class).toInstance(mockCloudAPIFactory))
      .build();
  }
}
