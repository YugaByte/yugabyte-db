// Copyright (c) YugaByte, Inc.

import java.lang.ReflectiveOperationException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import com.yugabyte.yw.commissioner.TaskGarbageCollector;
import com.yugabyte.yw.common.*;
import io.ebean.Ebean;
import com.google.common.collect.ImmutableMap;
import com.google.inject.Inject;
import com.google.inject.Singleton;
import com.yugabyte.yw.cloud.AWSInitializer;

import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.ExtraMigration;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.MetricConfig;
import com.yugabyte.yw.models.Provider;

import play.Application;
import play.Configuration;
import play.Environment;
import play.Logger;

import io.prometheus.client.hotspot.DefaultExports;

import static com.yugabyte.yw.common.ConfigHelper.ConfigType.SoftwareVersion;
import static com.yugabyte.yw.common.ConfigHelper.ConfigType.YugawareMetadata;


/**
 * We will use this singleton to do actions specific to the app environment, like
 * db seed etc.
 */
@Singleton
public class AppInit {

  @Inject
  public AppInit(Environment environment, Application application,
                 ConfigHelper configHelper, ReleaseManager releaseManager,
                 AWSInitializer awsInitializer, CustomerTaskManager taskManager,
                 YamlWrapper yaml, ExtraMigrationManager extraMigrationManager,
                 TaskGarbageCollector taskGC, PlatformBackupManager platformBackupManager
  ) throws ReflectiveOperationException {
    Logger.info("Yugaware Application has started");
    Configuration appConfig = application.configuration();
    String mode = appConfig.getString("yb.mode", "PLATFORM");

    if (!environment.isTest()) {
      // Check if we have provider data, if not, we need to seed the database
      if (Customer.find.query().where().findCount() == 0 &&
          appConfig.getBoolean("yb.seedData", false)) {
        Logger.debug("Seed the Yugaware DB");

        List<?> all = yaml.load(
            environment.resourceAsStream("db_seed.yml"),
            application.classloader()
        );
        Ebean.saveAll(all);
      }

      if (mode.equals("PLATFORM")) {
        String devopsHome = appConfig.getString("yb.devops.home");
        String storagePath = appConfig.getString("yb.storage.path");
        if (devopsHome == null || devopsHome.length() == 0) {
          throw new RuntimeException("yb.devops.home is not set in application.conf");
        }

        if (storagePath == null || storagePath.length() == 0) {
          throw new RuntimeException(("yb.storage.path is not set in application.conf"));
        }
      }

      // TODO: Version added to Yugaware metadata, now slowly decomission SoftwareVersion property
      String version = yaml.load(environment.resourceAsStream("version.txt"),
                                  application.classloader());
      configHelper.loadConfigToDB(SoftwareVersion, ImmutableMap.of("version", version));
      Map <String, Object> ywMetadata = new HashMap<>();
      // Assign a new Yugaware UUID if not already present in the DB i.e. first install
      Object ywUUID = configHelper.getConfig(YugawareMetadata)
                                  .getOrDefault("yugaware_uuid", UUID.randomUUID());
      ywMetadata.put("yugaware_uuid", ywUUID);
      ywMetadata.put("version", version);
      configHelper.loadConfigToDB(YugawareMetadata, ywMetadata);

      // Initialize AWS if any of its instance types have an empty volumeDetailsList
      List<Provider> providerList = Provider.find.query().where().findList();
      for (Provider provider : providerList) {
        if (provider.code.equals("aws")) {
          for (InstanceType instanceType : InstanceType.findByProvider(provider,
            application.config())) {
            if (instanceType.instanceTypeDetails != null &&
              (instanceType.instanceTypeDetails.volumeDetailsList == null)) {
              awsInitializer.initialize(provider.customerUUID, provider.uuid);
              break;
            }
          }
          break;
        }
      }

      // Load metrics configurations.
      Map<String, Object> configs = yaml.load(
          environment.resourceAsStream("metrics.yml"),
          application.classloader()
      );
      MetricConfig.loadConfig(configs);

      // Enter all the configuration data. This is the first thing that should be
      // done as the other init steps may depend on this data.
      configHelper.loadConfigsToDB(application);

      // Run and delete any extra migrations.
      for (ExtraMigration m: ExtraMigration.getAll()) {
        m.run(extraMigrationManager);
      }

      // Import new local releases into release metadata
      releaseManager.importLocalReleases();

      // initialize prometheus exports
      DefaultExports.initialize();

      // Fail incomplete tasks
      taskManager.failAllPendingTasks();

      // Schedule garbage collection of old completed tasks in database.
      taskGC.start();

      // TODO: (Daniel) - Integrate this with runtime settings once #5975 has landed
      // Start periodic platform backups
      platformBackupManager.start();

      // Add checksums for all certificates that don't have a checksum.
      CertificateHelper.createChecksums();

      Logger.info("AppInit completed");
   }
  }
}
