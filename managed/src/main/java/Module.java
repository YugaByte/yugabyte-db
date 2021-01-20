// Copyright (c) YugaByte, Inc.

import com.google.inject.AbstractModule;
import com.google.inject.Provides;
import com.yugabyte.yw.cloud.AWSInitializer;
import com.yugabyte.yw.cloud.aws.AWSCloudModule;
import com.yugabyte.yw.commissioner.*;
import com.yugabyte.yw.common.*;
import com.yugabyte.yw.common.kms.EncryptionAtRestManager;
import com.yugabyte.yw.common.kms.util.EncryptionAtRestUniverseKeyCache;
import com.yugabyte.yw.common.services.LocalYBClientService;
import com.yugabyte.yw.common.services.YBClientService;
import com.yugabyte.yw.controllers.PlatformHttpActionAdapter;
import com.yugabyte.yw.metrics.MetricQueryHelper;
import com.yugabyte.yw.queries.LiveQueryHelper;
import com.yugabyte.yw.scheduler.Scheduler;
import org.pac4j.core.client.Clients;
import org.pac4j.core.config.Config;
import org.pac4j.oidc.client.OidcClient;
import org.pac4j.oidc.config.OidcConfiguration;
import org.pac4j.play.CallbackController;
import org.pac4j.play.store.PlayCacheSessionStore;
import org.pac4j.play.store.PlaySessionStore;
import play.Configuration;
import play.Environment;

/**
 * This class is a Guice module that tells Guice to bind different types
 *
 * Play will automatically use any class called 'Module' in the root package
 */
public class Module extends AbstractModule {

  private final Environment environment;
  private final Configuration config;

  public Module(Environment environment, Configuration config) {
    this.environment = environment;
    this.config = config;
  }

  @Override
  public void configure() {
    // TODO: other clouds
    install(new AWSCloudModule());

    // Bind Application Initializer
    bind(AppInit.class).asEagerSingleton();
    bind(ConfigHelper.class).asEagerSingleton();
    // Set LocalClientService as the implementation for YBClientService
    bind(YBClientService.class).to(LocalYBClientService.class);
    bind(YsqlQueryExecutor.class).asEagerSingleton();
    bind(YcqlQueryExecutor.class).asEagerSingleton();
    bind(PlaySessionStore.class).to(PlayCacheSessionStore.class);

    // We only needed to bind below ones for Platform mode.
    if (config.getString("yb.mode", "PLATFORM").equals("PLATFORM")) {
      bind(SwamperHelper.class).asEagerSingleton();
      bind(HealthManager.class).asEagerSingleton();
      bind(NodeManager.class).asEagerSingleton();
      bind(MetricQueryHelper.class).asEagerSingleton();
      bind(LiveQueryHelper.class).asEagerSingleton();
      bind(ShellProcessHandler.class).asEagerSingleton();
      bind(NetworkManager.class).asEagerSingleton();
      bind(AccessManager.class).asEagerSingleton();
      bind(ReleaseManager.class).asEagerSingleton();
      bind(TemplateManager.class).asEagerSingleton();
      bind(ExtraMigrationManager.class).asEagerSingleton();
      bind(AWSInitializer.class).asEagerSingleton();
      bind(KubernetesManager.class).asEagerSingleton();
      bind(CallHome.class).asEagerSingleton();
      bind(Scheduler.class).asEagerSingleton();
      bind(HealthChecker.class).asEagerSingleton();
      bind(TaskGarbageCollector.class).asEagerSingleton();
      bind(EncryptionAtRestManager.class).asEagerSingleton();
      bind(EncryptionAtRestUniverseKeyCache.class).asEagerSingleton();
      bind(SetUniverseKey.class).asEagerSingleton();
      bind(CustomerTaskManager.class).asEagerSingleton();
      bind(YamlWrapper.class).asEagerSingleton();
      bind(AlertManager.class).asEagerSingleton();
      bind(QueryAlerts.class).asEagerSingleton();
      bind(PlatformBackupManager.class).asEagerSingleton();

      final CallbackController callbackController = new CallbackController();
      callbackController.setDefaultUrl(config.getString("yb.url", ""));
      bind(CallbackController.class).toInstance(callbackController);
    }
  }

  @Provides
  protected OidcClient provideOidcClient() {
    final OidcConfiguration oidcConfiguration = new OidcConfiguration();

    if (config.getString("yb.security.type", "").equals("OIDC")) {
      oidcConfiguration.setClientId(config.getString("yb.security.clientID", ""));
      oidcConfiguration.setSecret(config.getString("yb.security.secret", ""));
      oidcConfiguration.setScope(config.getString("yb.security.oidcScope", ""));
      oidcConfiguration.setDiscoveryURI(config.getString("yb.security.discoveryURI", ""));
      oidcConfiguration.setMaxClockSkew(3600);
      oidcConfiguration.setResponseType("code");
      final OidcClient oidcClient = new OidcClient(oidcConfiguration);
      return oidcClient;
    } else {
      return new OidcClient(oidcConfiguration);
    }
  }

  @Provides
  protected Config provideConfig(OidcClient oidcClient) {
    final Clients clients = new Clients(String.format("%s/api/v1/callback",
        config.getString("yb.url", "")), oidcClient);
    final Config config = new Config(clients);
    config.setHttpActionAdapter(new PlatformHttpActionAdapter());
    return config;
  }
}
