// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.google.inject.Inject;
import com.google.inject.Singleton;
import play.libs.Json;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Provider;

import static com.yugabyte.yw.commissioner.Common.CloudType.onprem;

/*
 * This is a manager to hold injected resources needed for extra migrations.
 */
@Singleton
public class ExtraMigrationManager extends DevopsBase {
  @Inject
  TemplateManager templateManager;

  @Override
  protected String getCommandType() {
    return "";
  }

  public void V52__Update_Access_Key_Create_Extra_Migration() {
    for (AccessKey accessKey: AccessKey.getAll()) {
      Provider p = Provider.get(accessKey.getProviderUUID());
      if (p != null && p.code.equals(onprem.name())) {
        AccessKey.KeyInfo keyInfo = accessKey.getKeyInfo();
        templateManager.createProvisionTemplate(
          accessKey, keyInfo.airGapInstall, keyInfo.passwordlessSudoAccess,
          keyInfo.installNodeExporter, keyInfo.nodeExporterPort, keyInfo.nodeExporterUser);
      }
    }
  }
}
