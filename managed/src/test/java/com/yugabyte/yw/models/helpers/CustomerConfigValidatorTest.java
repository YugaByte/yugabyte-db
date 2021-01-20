// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models.helpers;

import static org.junit.Assert.assertEquals;

import org.junit.Test;
import org.junit.runner.RunWith;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import junitparams.JUnitParamsRunner;
import junitparams.Parameters;
import play.libs.Json;

import play.api.Play;

@RunWith(JUnitParamsRunner.class)
public class CustomerConfigValidatorTest {

  private CustomerConfigValidator validator = new CustomerConfigValidator();

  @Test
  // @formatter:off
  @Parameters({
    "NFS, BACKUP_LOCATION, /tmp, true",
    "NFS, BACKUP_LOCATION, tmp, false",
    "NFS, BACKUP_LOCATION, /mnt/storage, true",
    "NFS, BACKUP_LOCATION, //, true",
    "NFS, BACKUP_LOCATION, $(ping -c1 google.com.ru > /tmp/ping_log)/tmp/some/nfs/dir, false",
    "NFS, BACKUP_LOCATION,, false",

    "S3, BACKUP_LOCATION, s3://backups.yugabyte.com/test/itest, true",
    "S3, AWS_HOST_BASE, s3://backups.yugabyte.com/test/itest, false",  // BACKUP_LOCATION undefined
    "S3, BACKUP_LOCATION, s3.amazonaws.com, true",
    "S3, BACKUP_LOCATION, ftp://s3.amazonaws.com, false",
    "S3, BACKUP_LOCATION,, false",

    "GCS, BACKUP_LOCATION, gs://itest-backup, true",
    "GCS, BACKUP_LOCATION, gcp.test.com, true",
    "GCS, BACKUP_LOCATION, ftp://gcp.test.com, false",
    "GCS, BACKUP_LOCATION,, false",

    "AZ, BACKUP_LOCATION, https://www.microsoft.com/azure, true",
    "AZ, BACKUP_LOCATION, http://www.microsoft.com/azure, true",
    "AZ, BACKUP_LOCATION, www.microsoft.com/azure, true",
    "AZ, BACKUP_LOCATION, ftp://www.microsoft.com/azure, false",
    "AZ, BACKUP_LOCATION,, false",
  })
  // @formatter:on
  public void testValidateDataContent_Storage_OneParamToCheck(String storageType, String fieldName,
      String fieldValue, boolean expectedResult) {
    ObjectNode data = Json.newObject().put(fieldName, fieldValue);
    ObjectNode result = validator.validateDataContent(createFormData("STORAGE", storageType, data));
    assertEquals(expectedResult, result.size() == 0);
  }

  @Test
  // @formatter:off
  @Parameters({
    // location - correct, aws_host_base - empty -> allowed
    "S3, BACKUP_LOCATION, s3://backups.yugabyte.com/test/itest, AWS_HOST_BASE,, true",
    // location - correct, aws_host_base - incorrect -> disallowed
    "S3, BACKUP_LOCATION, s3://backups.yugabyte.com/test/itest, " +
        "AWS_HOST_BASE, ftp://s3.amazonaws.com, false",
    // location - correct, aws_host_base - correct -> allowed
    "S3, BACKUP_LOCATION, s3://backups.yugabyte.com/test/itest, " +
        "AWS_HOST_BASE, s3.amazonaws.com, true",
    // location - correct, aws_host_base - correct -> allowed
    "S3, BACKUP_LOCATION, s3://backups.yugabyte.com/test/itest, " +
        "AWS_HOST_BASE, cloudstorage.onefs.dell.com, true",
    // location - incorrect, aws_host_base - correct -> disallowed
    "S3, BACKUP_LOCATION, ftp://backups.yugabyte.com/test/itest, " +
        "AWS_HOST_BASE, s3.amazonaws.com, false",
    // location - incorrect, aws_host_base - empty -> disallowed
    "S3, BACKUP_LOCATION, ftp://backups.yugabyte.com/test/itest, AWS_HOST_BASE,, false",
    // location - empty, aws_host_base - correct -> disallowed
    "S3, BACKUP_LOCATION,, AWS_HOST_BASE, s3.amazonaws.com, false",
    // location - empty, aws_host_base - empty -> disallowed
    "S3, BACKUP_LOCATION,, AWS_HOST_BASE,, false",
  })
  // @formatter:on
  public void testValidateDataContent_Storage_TwoParamsToCheck(String storageType,
      String fieldName1, String fieldValue1, String fieldName2, String fieldValue2,
      boolean expectedResult) {
    ObjectNode data = Json.newObject();
    data.put(fieldName1, fieldValue1);
    data.put(fieldName2, fieldValue2);
    ObjectNode result = validator.validateDataContent(createFormData("STORAGE", storageType, data));
    assertEquals(expectedResult, result.size() == 0);
  }

  private JsonNode createFormData(String type, String name, JsonNode data) {
    ObjectNode formData = Json.newObject();
    formData.put("type", type);
    formData.put("name", name);
    formData.put("data", data);
    return formData;
  }
}
