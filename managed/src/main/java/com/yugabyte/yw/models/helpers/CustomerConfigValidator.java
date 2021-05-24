// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models.helpers;

import com.amazonaws.services.s3.model.Bucket;
import com.amazonaws.services.s3.model.AmazonS3Exception;
import com.amazonaws.services.s3.AmazonS3;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.inject.Singleton;
import com.yugabyte.yw.forms.PasswordPolicyFormData;
import com.yugabyte.yw.models.CustomerConfig;
import org.apache.commons.lang3.StringUtils;
import org.apache.commons.validator.routines.UrlValidator;
import play.libs.Json;
import javax.inject.Inject;
import javax.validation.ConstraintViolation;
import javax.validation.Validator;
import java.net.URI;
import java.net.URISyntaxException;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.HashSet;
import java.util.regex.Pattern;
import com.amazonaws.services.s3.AmazonS3ClientBuilder;
import com.amazonaws.auth.AWSStaticCredentialsProvider;
import com.amazonaws.auth.BasicAWSCredentials;
import com.amazonaws.auth.AWSCredentials;
import com.amazonaws.regions.Regions;

import static com.yugabyte.yw.models.CustomerConfig.ConfigType.PASSWORD_POLICY;
import static com.yugabyte.yw.models.CustomerConfig.ConfigType.STORAGE;

@Singleton
public class CustomerConfigValidator {

  private static final String NAME_S3 = "S3";

  private static final String NAME_GCS = "GCS";

  private static final String NAME_NFS = "NFS";

  private static final String NAME_AZURE = "AZ";

  private static final String[] S3_URL_SCHEMES = {"http", "https", "s3"};

  private static final String[] GCS_URL_SCHEMES = {"http", "https", "gs"};

  private static final String[] AZ_URL_SCHEMES = {"http", "https"};

  private static final String AWS_HOST_BASE_FIELDNAME = "AWS_HOST_BASE";

  private static final String BACKUP_LOCATION_FIELDNAME = "BACKUP_LOCATION";

  private static final String NFS_PATH_REGEXP = "^/|//|(/[\\w-]+)+$";

  private final Validator validator;

  public abstract static class ConfigValidator {

    protected final String type;

    protected final String name;

    public ConfigValidator(String type, String name) {
      this.type = type;
      this.name = name;
    }

    public void validate(String type, String name, JsonNode data, ObjectNode errorsCollector) {
      if (this.type.equals(type) && this.name.equals(name)) {
        doValidate(data, errorsCollector);
      }
    }

    protected abstract void doValidate(JsonNode data, ObjectNode errorsCollector);
  }

  public abstract static class ConfigFieldValidator extends ConfigValidator {

    protected final String fieldName;

    public ConfigFieldValidator(String type, String name, String fieldName) {
      super(type, name);
      this.fieldName = fieldName;
    }

    @Override
    public void doValidate(JsonNode data, ObjectNode errorsCollector) {
      JsonNode value = data.get(fieldName);
      doValidate(value == null ? "" : value.asText(), errorsCollector);
    }

    protected abstract void doValidate(String value, ObjectNode errorsCollector);
  }

  public static class ConfigS3PreflightCheckValidator extends ConfigValidator {

    protected final String fieldName;

    public ConfigS3PreflightCheckValidator(String type, String name, String fieldName) {
      super(type, name);
      this.fieldName = fieldName;
    }

    @Override
    public void doValidate(JsonNode data, ObjectNode errorJson) {
      if (this.name.equals("S3") && data.get("AWS_ACCESS_KEY_ID") != null) {
        String bucketname = data.get("BACKUP_LOCATION").asText();
        //Assuming bucket name will always start with s3:// otherwise that will be invalid
        if (bucketname.length() < 5 || !bucketname.startsWith("s3://")) {
          errorJson.set(fieldName, Json.newArray().add("Invalid bucket name: " + bucketname));
        } else {
          try {
            bucketname = bucketname.substring(5);
            AmazonS3 s3client = create(data.get("AWS_ACCESS_KEY_ID").asText(),
              data.get("AWS_SECRET_ACCESS_KEY").asText());
            List<Bucket> buckets = s3client.listBuckets();
            final HashSet<String> bucketList = new HashSet<String>();
            for (Bucket bucket : buckets) {
              bucketList.add(bucket.getName());
            }
            if (!bucketList.contains(bucketname))
              errorJson.set(fieldName, Json.newArray().add("Bucket name " + bucketname +
                " doesn't exist"));
          } catch(AmazonS3Exception s3Exception) {
            errorJson.set(fieldName, Json.newArray().add(s3Exception.getErrorMessage()));
          }
        }
      }
    }
  }

  public class ConfigObjectValidator<T> extends ConfigValidator {
    private Class<T> configClass;

    public ConfigObjectValidator(String type, String name, Class<T> configClass) {
      super(type, name);
      this.configClass = configClass;
    }

    @Override
    protected void doValidate(JsonNode data, ObjectNode errorsCollector) {
      ObjectMapper mapper = new ObjectMapper();
      try {
        T config = mapper.treeToValue(data, configClass);
        Set<ConstraintViolation<T>> violations = validator.validate(config);
        if (!violations.isEmpty()) {
          ArrayNode errors = Json.newArray();
          violations.stream().map(ConstraintViolation::getMessage).forEach(errors::add);
          errorsCollector.set(name, errors);
        }
      } catch (RuntimeException | JsonProcessingException e) {
        errorsCollector.set(
            name,
            Json.newArray().add("Invalid json for type '" + configClass.getSimpleName() + "'."));
      }
    }
  }

  public static class ConfigValidatorRegEx extends ConfigFieldValidator {

    private Pattern pattern;

    public ConfigValidatorRegEx(String type, String name, String fieldName, String regex) {
      super(type, name, fieldName);
      pattern = Pattern.compile(regex);
    }

    @Override
    protected void doValidate(String value, ObjectNode errorsCollector) {
      if (!pattern.matcher(value).matches()) {
        errorsCollector.set(fieldName, Json.newArray().add("Invalid field value '" + value + "'."));
      }
    }
  }

  public static class ConfigValidatorUrl extends ConfigFieldValidator {

    private static final String DEFAULT_SCHEME = "https://";

    private final UrlValidator urlValidator;

    private final boolean emptyAllowed;

    public ConfigValidatorUrl(
        String type, String name, String fieldName, String[] schemes, boolean emptyAllowed) {
      super(type, name, fieldName);
      this.emptyAllowed = emptyAllowed;
      urlValidator = new UrlValidator(schemes, UrlValidator.ALLOW_LOCAL_URLS);
    }

    @Override
    protected void doValidate(String value, ObjectNode errorsCollector) {
      if (StringUtils.isEmpty(value)) {
        if (!emptyAllowed) {
          errorsCollector.set(fieldName, Json.newArray().add("This field is required."));
        }
        return;
      }

      boolean valid = false;
      try {
        URI uri = new URI(value);
        valid =
            urlValidator.isValid(
                StringUtils.isEmpty(uri.getScheme()) ? DEFAULT_SCHEME + value : value);
      } catch (URISyntaxException e) {
      }

      if (!valid) {
        errorsCollector.set(fieldName, Json.newArray().add("Invalid field value '" + value + "'."));
      }
    }
  }

  private final List<ConfigValidator> validators = new ArrayList<>();

  @Inject
  public CustomerConfigValidator(Validator validator) {
    this.validator = validator;
    validators.add(
        new ConfigValidatorRegEx(
            STORAGE.name(), NAME_NFS, BACKUP_LOCATION_FIELDNAME, NFS_PATH_REGEXP));
    validators.add(
        new ConfigValidatorUrl(
            STORAGE.name(), NAME_S3, BACKUP_LOCATION_FIELDNAME, S3_URL_SCHEMES, false));
    validators.add(
        new ConfigValidatorUrl(
            STORAGE.name(), NAME_S3, AWS_HOST_BASE_FIELDNAME, S3_URL_SCHEMES, true));
    validators.add(
        new ConfigValidatorUrl(
            STORAGE.name(), NAME_GCS, BACKUP_LOCATION_FIELDNAME, GCS_URL_SCHEMES, false));
    validators.add(
        new ConfigValidatorUrl(
            STORAGE.name(), NAME_AZURE, BACKUP_LOCATION_FIELDNAME, AZ_URL_SCHEMES, false));
    validators.add(
        new ConfigObjectValidator<>(
            PASSWORD_POLICY.name(), CustomerConfig.PASSWORD_POLICY, PasswordPolicyFormData.class));
  }

  public ObjectNode validateFormData(JsonNode formData) {
    ObjectNode errorJson = Json.newObject();
    if (!formData.hasNonNull("name")) {
      errorJson.set("name", Json.newArray().add("This field is required"));
    }
    if (!formData.hasNonNull("type")) {
      errorJson.set("type", Json.newArray().add("This field is required"));
    } else if (!CustomerConfig.ConfigType.isValid(formData.get("type").asText())) {
      errorJson.set("type", Json.newArray().add("Invalid type provided"));
    }

    if (!formData.hasNonNull("data")) {
      errorJson.set("data", Json.newArray().add("This field is required"));
    } else if (!formData.get("data").isObject()) {
      errorJson.set("data", Json.newArray().add("Invalid data provided, expected a object."));
    }
    return errorJson;
  }

  /**
   * Validates data which is contained in formData. During the procedure it calls all the registered
   * validators. Errors are collected and returned back as a result. Empty result object means no
   * errors.
   *
   * <p>Currently are checked: - NFS - NFS Storage Path (against regexp NFS_PATH_REGEXP); - S3/AWS -
   * S3 Bucket, S3 Bucket Host Base (both as URLs); - GCS - GCS Bucket (as URL); - AZURE - Container
   * URL (as URL).
   *
   * <p>The URLs validation allows empty scheme. In such case the check is made with DEFAULT_SCHEME
   * added before the URL.
   *
   * @param formData
   * @return Json filled with errors
   */
  public ObjectNode validateDataContent(JsonNode formData) {
    ObjectNode errorJson = Json.newObject();
    validators.forEach(
        v ->
            v.validate(
                formData.get("type").asText(),
                formData.get("name").asText(),
                formData.get("data"),
                errorJson));
    return errorJson;
  }

  //TODO: move this out to some common util file.
  public static AmazonS3 create(String key, String secret) {
    AWSCredentials credentials = new BasicAWSCredentials(
      key,
      secret
    );
    return AmazonS3ClientBuilder
      .standard()
      .withCredentials(new AWSStaticCredentialsProvider(credentials))
      .withRegion(Regions.DEFAULT_REGION)
      .build();
  }
}
