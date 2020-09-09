package com.yugabyte.yw.controllers;

import com.google.inject.Inject;

import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.common.CertificateHelper;
import com.yugabyte.yw.models.Audit;
import com.yugabyte.yw.models.CertificateInfo;
import com.yugabyte.yw.models.Customer;
import com.yugabyte.yw.forms.CertificateParams;
import com.yugabyte.yw.forms.ClientCertParams;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.mvc.Result;
import play.data.Form;
import play.data.FormFactory;
import play.libs.Json;

import java.util.Date;
import java.util.List;
import java.util.UUID;
import java.util.stream.Collectors;

public class CertificateController extends AuthenticatedController {
  public static final Logger LOG = LoggerFactory.getLogger(CertificateController.class);

  @Inject
  play.Configuration appConfig;

  @Inject
  FormFactory formFactory;

  public Result upload(UUID customerUUID) {
    Form<CertificateParams> formData = formFactory.form(CertificateParams.class)
                                                  .bindFromRequest();
    if (Customer.get(customerUUID) == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    Date certStart = new Date(formData.get().certStart);
    Date certExpiry = new Date(formData.get().certExpiry);
    String label = formData.get().label;
    String certContent = formData.get().certContent;
    String keyContent = formData.get().keyContent;
    try {
      UUID certUUID = CertificateHelper.uploadRootCA(label, customerUUID, appConfig.getString("yb.storage.path"),
          certContent, keyContent, certStart, certExpiry);
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
      return ApiResponse.success(certUUID);
    } catch (Exception e) {
      return ApiResponse.error(BAD_REQUEST, "Couldn't upload certfiles");
    }
  }

  public Result getClientCert(UUID customerUUID, UUID rootCA) {
    Form<ClientCertParams> formData = formFactory.form(ClientCertParams.class)
                                                 .bindFromRequest();
    if (Customer.get(customerUUID) == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    if (formData.hasErrors()) {
      return ApiResponse.error(BAD_REQUEST, formData.errorsAsJson());
    }
    Date certStart = new Date(formData.get().certStart);
    Date certExpiry = new Date(formData.get().certExpiry);
    try {
      JsonNode result = CertificateHelper.createClientCertificate(
          rootCA, null, formData.get().username, certStart, certExpiry);
      Audit.createAuditEntry(ctx(), request(), Json.toJson(formData.data()));
      return ApiResponse.success(result);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Couldn't generate client cert.");
    }
  }

  public Result getRootCert(UUID customerUUID, UUID rootCA) {
    if (Customer.get(customerUUID) == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    if (CertificateInfo.get(rootCA) == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Cert ID: " + rootCA);
    }
    if (!CertificateInfo.get(rootCA).customerUUID.equals(customerUUID)) {
      return ApiResponse.error(BAD_REQUEST, "Certificate doesn't belong to customer");
    }
    try {
      String certContents = CertificateHelper.getCertPEMFileContents(rootCA);
      Audit.createAuditEntry(ctx(), request());
      ObjectNode result = Json.newObject();
      result.put(CertificateHelper.ROOT_CERT, certContents);
      return ApiResponse.success(result);
    } catch (Exception e) {
      return ApiResponse.error(INTERNAL_SERVER_ERROR, "Couldn't fetch root cert.");
    }
  }

  public Result list(UUID customerUUID) {
    List<CertificateInfo> certs = CertificateInfo.getAll(customerUUID);
    if (certs == null) {
      return ApiResponse.error(BAD_REQUEST, "Invalid Customer UUID: " + customerUUID);
    }
    return ApiResponse.success(certs);
  }

  public Result get(UUID customerUUID, String label) {
    CertificateInfo cert = CertificateInfo.get(label);
    if (cert == null) {
      return ApiResponse.error(BAD_REQUEST, "No Certificate with Label: " + label);
    } else {
      return ApiResponse.success(cert.uuid);
    }
  }
}
