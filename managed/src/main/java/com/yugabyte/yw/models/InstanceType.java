// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.models;

import com.fasterxml.jackson.annotation.JsonBackReference;
import com.fasterxml.jackson.databind.JsonNode;
import com.google.common.collect.ImmutableList;
import com.typesafe.config.Config;
import com.yugabyte.yw.common.YWServiceException;
import com.yugabyte.yw.cloud.PublicCloudConstants;
import io.ebean.Ebean;
import io.ebean.Finder;
import io.ebean.Model;
import io.ebean.SqlUpdate;
import io.ebean.annotation.EnumValue;
import io.swagger.annotations.ApiModel;
import io.swagger.annotations.ApiModelProperty;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.validation.Constraints;
import play.libs.Json;

import javax.persistence.*;
import java.util.LinkedList;
import java.util.List;
import java.util.UUID;
import java.util.function.Predicate;
import java.util.stream.Collectors;

import static play.mvc.Http.Status.BAD_REQUEST;
import static io.swagger.annotations.ApiModelProperty.AccessMode.*;

@ApiModel(description = "Instance type model which holds the information about the instance.")
@Entity
public class InstanceType extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(InstanceType.class);

  public static List<String> AWS_INSTANCE_PREFIXES_SUPPORTED =
      ImmutableList.of("m3.", "c5.", "c5d.", "c4.", "c3.", "i3.", "t2.");
  static final String YB_AWS_DEFAULT_VOLUME_COUNT_KEY = "yb.aws.default_volume_count";
  static final String YB_AWS_DEFAULT_VOLUME_SIZE_GB_KEY = "yb.aws.default_volume_size_gb";

  public enum VolumeType {
    @EnumValue("EBS")
    EBS,

    @EnumValue("SSD")
    SSD,

    @EnumValue("HDD")
    HDD,

    @EnumValue("NVME")
    NVME
  }

  @EmbeddedId @Constraints.Required public InstanceTypeKey idKey;

  // ManyToOne for provider is kept outside of InstanceTypeKey
  // as ebean currently doesn't support having @ManyToOne inside @EmbeddedId
  // insertable and updatable are set to false as actual updates
  // are taken care by providerUuid parameter in InstanceTypeKey
  @ManyToOne(optional = false)
  @JoinColumn(name = "provider_uuid", insertable = false, updatable = false)
  private Provider provider;

  public Provider getProvider() {
    if (this.provider == null) {
      setProviderUuid(this.idKey.providerUuid);
    }
    return this.provider;
  }

  public void setProvider(Provider aProvider) {
    provider = aProvider;
    idKey.providerUuid = aProvider.uuid;
  }

  public UUID getProviderUuid() {
    return this.idKey.providerUuid;
  }

  public void setProviderUuid(UUID providerUuid) {
    Provider provider = Provider.get(providerUuid);
    if (provider != null) {
      setProvider(provider);
    } else {
      LOG.error("No provider found for the given id: {}", providerUuid);
    }
  }

  public String getProviderCode() {
    Provider provider = getProvider();
    return provider != null ? provider.code : null;
  }

  public String getInstanceTypeCode() {
    return this.idKey.instanceTypeCode;
  }

  public void setInstanceTypeCode(String code) {
    idKey.instanceTypeCode = code;
  }

  @ApiModelProperty(
      value = "indiacates whether this instance is active or not",
      accessMode = READ_ONLY)
  @Constraints.Required
  @Column(nullable = false, columnDefinition = "boolean default true")
  private Boolean active = true;

  public Boolean isActive() {
    return active;
  }

  public void setActive(Boolean active) {
    this.active = active;
  }

  @ApiModelProperty(value = "Number of cores in an instance", accessMode = READ_WRITE)
  @Constraints.Required
  @Column(nullable = false, columnDefinition = "float")
  public Double numCores;

  @ApiModelProperty(value = "Memory size of an instance", accessMode = READ_WRITE)
  @Constraints.Required
  @Column(nullable = false, columnDefinition = "float")
  public Double memSizeGB;

  @ApiModelProperty(value = "Extra details about instance json", accessMode = READ_WRITE)
  @Column(columnDefinition = "TEXT")
  private String instanceTypeDetailsJson;

  public InstanceTypeDetails instanceTypeDetails;

  private static final Finder<InstanceTypeKey, InstanceType> find =
      new Finder<InstanceTypeKey, InstanceType>(InstanceType.class) {};

  public static InstanceType get(UUID providerUuid, String instanceTypeCode) {
    InstanceType instanceType = find.byId(InstanceTypeKey.create(instanceTypeCode, providerUuid));
    if (instanceType == null) {
      return instanceType;
    }
    // Since 'instanceTypeDetailsJson' can be null (populated externally), we need to populate these
    // fields explicitly.
    if (instanceType.instanceTypeDetailsJson == null
        || instanceType.instanceTypeDetailsJson.isEmpty()) {
      instanceType.instanceTypeDetails = new InstanceTypeDetails();
      instanceType.instanceTypeDetailsJson =
          Json.stringify(Json.toJson(instanceType.instanceTypeDetails));
    } else {
      instanceType.instanceTypeDetails =
          Json.fromJson(
              Json.parse(instanceType.instanceTypeDetailsJson), InstanceTypeDetails.class);
    }
    return instanceType;
  }

  public static InstanceType getOrBadRequest(UUID providerUuid, String instanceTypeCode) {
    InstanceType instanceType = InstanceType.get(providerUuid, instanceTypeCode);
    if (instanceType == null) {
      throw new YWServiceException(BAD_REQUEST, "Instance Type not found: " + instanceTypeCode);
    }
    return instanceType;
  }

  public static InstanceType upsert(
      UUID providerUuid,
      String instanceTypeCode,
      Integer numCores,
      Double memSize,
      InstanceTypeDetails instanceTypeDetails) {
    return upsert(providerUuid, instanceTypeCode, (double) numCores, memSize, instanceTypeDetails);
  }

  public static InstanceType upsert(
      UUID providerUuid,
      String instanceTypeCode,
      Double numCores,
      Double memSize,
      InstanceTypeDetails instanceTypeDetails) {
    InstanceType instanceType = InstanceType.get(providerUuid, instanceTypeCode);
    if (instanceType == null) {
      instanceType = new InstanceType();
      instanceType.idKey = InstanceTypeKey.create(instanceTypeCode, providerUuid);
    }
    instanceType.memSizeGB = memSize;
    instanceType.numCores = numCores;
    instanceType.instanceTypeDetails = instanceTypeDetails;
    instanceType.instanceTypeDetailsJson = Json.stringify(Json.toJson(instanceTypeDetails));
    // Update the in-memory fields.
    instanceType.save();
    // Update the JSON field - this does not seem to be updated by the save above.
    String updateQuery =
        "UPDATE instance_type "
            + "SET instance_type_details_json = :instanceTypeDetails "
            + "WHERE provider_uuid = :providerUuid AND instance_type_code = :instanceTypeCode";
    SqlUpdate update = Ebean.createSqlUpdate(updateQuery);
    update.setParameter("instanceTypeDetails", instanceType.instanceTypeDetailsJson);
    update.setParameter("providerUuid", providerUuid);
    update.setParameter("instanceTypeCode", instanceTypeCode);
    int modifiedCount = Ebean.execute(update);
    // Check if the save was not successful.
    if (modifiedCount == 0) {
      // Throw an exception as the save was not successful.
      LOG.error("Failed to update SQL row");
    } else if (modifiedCount > 1) {
      // Exactly one row should have been modified.
      LOG.error("Running query [" + updateQuery + "] updated " + modifiedCount + " rows");
    }
    return instanceType;
  }

  /**
   * Reset the 'instance_type_details_json' of all rows belonging to a specific provider in this
   * table.
   */
  public static void resetInstanceTypeDetailsForProvider(UUID providerUuid) {
    String updateQuery =
        "UPDATE instance_type "
            + "SET instance_type_details_json = '' WHERE provider_uuid = :providerUuid";
    SqlUpdate update =
        Ebean.createSqlUpdate(updateQuery).setParameter("providerUuid", providerUuid);
    int modifiedCount = Ebean.execute(update);
    LOG.info("Query [" + updateQuery + "] updated " + modifiedCount + " rows");
    if (modifiedCount == 0) {
      LOG.warn("Failed to update any SQL row");
    }
  }

  /** Delete Instance Types corresponding to given provider */
  public static void deleteInstanceTypesForProvider(Provider provider, Config config) {
    for (InstanceType instanceType : findByProvider(provider, config)) {
      instanceType.delete();
    }
  }

  private static Predicate<InstanceType> supportedInstanceTypes(List<String> supportedPrefixes) {
    return p ->
        supportedPrefixes.stream().anyMatch(prefix -> p.getInstanceTypeCode().startsWith(prefix));
  }

  private static List<InstanceType> populateDefaultsIfEmpty(
      List<InstanceType> entries, Config config) {
    // For AWS, we would filter and show only supported instance prefixes
    entries =
        entries
            .stream()
            .filter(supportedInstanceTypes(AWS_INSTANCE_PREFIXES_SUPPORTED))
            .collect(Collectors.toList());
    for (InstanceType instanceType : entries) {
      JsonNode parsedJson = Json.parse(instanceType.instanceTypeDetailsJson);
      if (parsedJson == null || parsedJson.isNull()) {
        instanceType.instanceTypeDetails = new InstanceTypeDetails();
      } else {
        instanceType.instanceTypeDetails = Json.fromJson(parsedJson, InstanceTypeDetails.class);
      }
      if (instanceType.instanceTypeDetails.volumeDetailsList.isEmpty()) {
        instanceType.instanceTypeDetails.setVolumeDetailsList(
            config.getInt(YB_AWS_DEFAULT_VOLUME_COUNT_KEY),
            config.getInt(YB_AWS_DEFAULT_VOLUME_SIZE_GB_KEY),
            VolumeType.EBS);
      }
    }
    return entries;
  }

  /** Query Helper to find supported instance types for a given cloud provider. */
  public static List<InstanceType> findByProvider(Provider provider, Config config) {
    List<InstanceType> entries =
        InstanceType.find
            .query()
            .where()
            .eq("provider_uuid", provider.uuid)
            .eq("active", true)
            .findList();
    if (provider.code.equals("aws")) {
      return populateDefaultsIfEmpty(entries, config);
    } else {
      return entries
          .stream()
          .map(entry -> InstanceType.get(entry.getProviderUuid(), entry.getInstanceTypeCode()))
          .collect(Collectors.toList());
    }
  }

  public static InstanceType createWithMetadata(
      UUID providerUuid, String instanceTypeCode, JsonNode metadata) {
    return upsert(
        providerUuid,
        instanceTypeCode,
        Integer.parseInt(metadata.get("numCores").toString()),
        Double.parseDouble(metadata.get("memSizeGB").toString()),
        Json.fromJson(metadata.get("instanceTypeDetails"), InstanceTypeDetails.class));
  }

  /** Default details for volumes attached to this instance. */
  public static class VolumeDetails {
    public Integer volumeSizeGB;
    public VolumeType volumeType;
    public String mountPath;
  }

  public static class InstanceTypeDetails {
    public static final int DEFAULT_VOLUME_COUNT = 1;
    public static final int DEFAULT_GCP_VOLUME_SIZE_GB = 375;
    public static final int DEFAULT_AZU_VOLUME_SIZE_GB = 250;

    public List<VolumeDetails> volumeDetailsList;
    public PublicCloudConstants.Tenancy tenancy;

    public InstanceTypeDetails() {
      volumeDetailsList = new LinkedList<>();
    }

    public void setVolumeDetailsList(int volumeCount, int volumeSizeGB, VolumeType volumeType) {
      for (int i = 0; i < volumeCount; i++) {
        VolumeDetails volumeDetails = new VolumeDetails();
        volumeDetails.volumeSizeGB = volumeSizeGB;
        volumeDetails.volumeType = volumeType;
        volumeDetailsList.add(volumeDetails);
      }
      setDefaultMountPaths();
    }

    public void setDefaultMountPaths() {
      for (int idx = 0; idx < volumeDetailsList.size(); ++idx) {
        volumeDetailsList.get(idx).mountPath = String.format("/mnt/d%d", idx);
      }
    }

    public static InstanceTypeDetails createGCPDefault() {
      InstanceTypeDetails instanceTypeDetails = new InstanceTypeDetails();
      instanceTypeDetails.setVolumeDetailsList(
          DEFAULT_VOLUME_COUNT, DEFAULT_GCP_VOLUME_SIZE_GB, VolumeType.SSD);
      return instanceTypeDetails;
    }

    public static InstanceTypeDetails createAZUDefault() {
      InstanceTypeDetails instanceTypeDetails = new InstanceTypeDetails();
      instanceTypeDetails.setVolumeDetailsList(
          DEFAULT_VOLUME_COUNT, DEFAULT_AZU_VOLUME_SIZE_GB, VolumeType.SSD);
      return instanceTypeDetails;
    }

    public static InstanceTypeDetails createGCPInstanceTypeDetails(VolumeType volumeType) {
      InstanceTypeDetails instanceTypeDetails = new InstanceTypeDetails();
      instanceTypeDetails.setVolumeDetailsList(
          DEFAULT_VOLUME_COUNT, DEFAULT_GCP_VOLUME_SIZE_GB, volumeType);
      return instanceTypeDetails;
    }
  }
}
