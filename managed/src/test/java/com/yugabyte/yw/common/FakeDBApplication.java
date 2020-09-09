// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.common;

import com.yugabyte.yw.common.YamlWrapper;
import com.yugabyte.yw.commissioner.CallHome;
import com.yugabyte.yw.commissioner.Commissioner;
import com.yugabyte.yw.commissioner.HealthChecker;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.commissioner.SetUniverseKey;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import play.Application;
import play.inject.guice.GuiceApplicationBuilder;
import play.test.Helpers;
import play.test.WithApplication;

import org.pac4j.play.CallbackController;
import org.pac4j.play.store.PlayCacheSessionStore;
import org.pac4j.play.store.PlaySessionStore;

import java.util.Map;
import java.util.concurrent.Executors;

import static org.mockito.Mockito.mock;
import static play.inject.Bindings.bind;

public class FakeDBApplication extends WithApplication {
  public Commissioner mockCommissioner;
  public CallHome mockCallHome;
  public ApiHelper mockApiHelper;
  public HealthChecker mockHealthChecker;
  public EncryptionAtRestManager mockEARManager;
  public SetUniverseKey mockSetUniverseKey;
  public CallbackController mockCallbackController;
  public PlayCacheSessionStore mockSessionStore;
  public AccessManager mockAccessManager;
  public TemplateManager mockTemplateManager;
  public MetricQueryHelper mockMetricQueryHelper;
  public CloudQueryHelper mockCloudQueryHelper;
  public ReleaseManager mockReleaseManager;
  public YBClientService mockService;
  public DnsManager mockDnsManager;
  public NetworkManager mockNetworkManager;
  public YamlWrapper mockYamlWrapper;

  @Override
  protected Application provideApplication() {
    mockEARManager = mock(EncryptionAtRestManager.class);
    mockApiHelper = mock(ApiHelper.class);
    mockCommissioner = mock(Commissioner.class);
    mockCallHome = mock(CallHome.class);
    mockSetUniverseKey = mock(SetUniverseKey.class);
    Executors mockExecutors = mock(Executors.class);
    mockCallbackController = mock(CallbackController.class);
    mockSessionStore = mock(PlayCacheSessionStore.class);
    mockAccessManager = mock(AccessManager.class);
    mockTemplateManager = mock(TemplateManager.class);
    mockMetricQueryHelper = mock(MetricQueryHelper.class);
    mockCloudQueryHelper = mock(CloudQueryHelper.class);
    mockReleaseManager = mock(ReleaseManager.class);
    mockService = mock(YBClientService.class);
    mockNetworkManager = mock(NetworkManager.class);
    mockDnsManager = mock(DnsManager.class);
    mockYamlWrapper = mock(YamlWrapper.class);
    return new GuiceApplicationBuilder()
        .configure((Map) Helpers.inMemoryDatabase())
        .overrides(bind(ApiHelper.class).toInstance(mockApiHelper))
        .overrides(bind(Commissioner.class).toInstance(mockCommissioner))
        .overrides(bind(CallHome.class).toInstance(mockCallHome))
        .overrides(bind(HealthChecker.class).toInstance(mockHealthChecker))
        .overrides(bind(Executors.class).toInstance(mockExecutors))
        .overrides(bind(EncryptionAtRestManager.class).toInstance(mockEARManager))
        .overrides(bind(SetUniverseKey.class).toInstance(mockSetUniverseKey))
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
        .build();
  }
}
