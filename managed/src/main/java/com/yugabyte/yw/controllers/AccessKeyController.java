// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.controllers;

import com.google.inject.Inject;
import com.yugabyte.yw.common.AccessManager;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.common.TemplateManager;
import com.yugabyte.yw.common.ValidatingFormFactory;
import com.yugabyte.yw.common.YWServiceException;
import com.yugabyte.yw.forms.AccessKeyFormData;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.AccessKey;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;
import play.mvc.Http;
import play.mvc.Result;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.UUID;

import static com.yugabyte.yw.commissioner.Common.CloudType.onprem;

public class AccessKeyController extends AuthenticatedController {

  @Inject
  ValidatingFormFactory formFactory;

  @Inject
  AccessManager accessManager;

  @Inject
  TemplateManager templateManager;

  public static final Logger LOG = LoggerFactory.getLogger(AccessKeyController.class);

  public Result index(UUID customerUUID, UUID providerUUID, String keyCode) {
    Customer.getOrBadRequest(customerUUID);
    Provider.getOrBadRequest(customerUUID, providerUUID);

    AccessKey accessKey = AccessKey.getOrBadRequest(providerUUID, keyCode);
    return ApiResponse.success(accessKey);
  }

  public Result list(UUID customerUUID, UUID providerUUID) {
    Customer.getOrBadRequest(customerUUID);
    Provider.getOrBadRequest(customerUUID, providerUUID);

    List<AccessKey> accessKeys;
    accessKeys = AccessKey.getAll(providerUUID);
    return ApiResponse.success(accessKeys);
  }

  public Result create(UUID customerUUID, UUID providerUUID) throws IOException {
    Form<AccessKeyFormData> formData = formFactory.getFormDataOrBadRequest(AccessKeyFormData.class);

    UUID regionUUID = formData.get().regionUUID;
    Region region = Region.getOrBadRequest(customerUUID, providerUUID, regionUUID);

    String keyCode = formData.get().keyCode;
    String keyContent = formData.get().keyContent;
    AccessManager.KeyType keyType = formData.get().keyType;
    String sshUser =  formData.get().sshUser;
    Integer sshPort =  formData.get().sshPort;
    boolean airGapInstall = formData.get().airGapInstall;
    boolean skipProvisioning = formData.get().skipProvisioning;
    AccessKey accessKey;

    LOG.info(
      "Creating access key {} for customer {}, provider {}.",
      keyCode, customerUUID, providerUUID
    );

    // Check if a public/private key was uploaded as part of the request
    Http.MultipartFormData multiPartBody = request().body().asMultipartFormData();
    if (multiPartBody != null) {
      Http.MultipartFormData.FilePart filePart = multiPartBody.getFile("keyFile");
      File uploadedFile = (File) filePart.getFile();
      if (keyType == null || uploadedFile == null) {
        return ApiResponse.error(BAD_REQUEST, "keyType and keyFile params required.");
      }
      accessKey = accessManager.uploadKeyFile(
          region.uuid, uploadedFile, keyCode, keyType, sshUser, sshPort, airGapInstall,
          skipProvisioning);
    } else if (keyContent != null && !keyContent.isEmpty()) {
      if (keyType == null) {
        return ApiResponse.error(BAD_REQUEST, "keyType params required.");
      }
      // Create temp file and fill with content
      Path tempFile = Files.createTempFile(keyCode, keyType.getExtension());
      Files.write(tempFile, keyContent.getBytes());

      // Upload temp file to create the access key and return success/failure
      accessKey = accessManager.uploadKeyFile(
          regionUUID, tempFile.toFile(), keyCode, keyType, sshUser, sshPort, airGapInstall,
          skipProvisioning);
    } else {
      accessKey = accessManager.addKey(
          regionUUID, keyCode, sshPort, airGapInstall, skipProvisioning);
    }

    // In case of onprem provider, we add a couple of additional attributes like passwordlessSudo
    // and create a preprovision script
    if (region.provider.code.equals(onprem.name())) {
      templateManager.createProvisionTemplate(
        accessKey,
        airGapInstall,
        formData.get().passwordlessSudoAccess,
        formData.get().installNodeExporter,
        formData.get().nodeExporterPort,
        formData.get().nodeExporterUser
      );
    }
    Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
    return ApiResponse.success(accessKey);
  }

  public Result delete(UUID customerUUID, UUID providerUUID, String keyCode) {
    Customer.getOrBadRequest(customerUUID);
    Provider.getOrBadRequest(customerUUID, providerUUID);
    AccessKey accessKey = AccessKey.getOrBadRequest(providerUUID, keyCode);
    LOG.info(
      "Deleting access key {} for customer {}, provider {}",
      keyCode, customerUUID, providerUUID
    );

    accessKey.deleteOrThrow(); 
    Audit.createAuditEntry(ctx(), request());
    return ApiResponse.success("Deleted KeyCode: " + keyCode);
  }
}
