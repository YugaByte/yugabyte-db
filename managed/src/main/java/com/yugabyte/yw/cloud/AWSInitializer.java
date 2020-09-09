/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */


package com.yugabyte.yw.cloud;

import com.fasterxml.jackson.databind.ObjectMapper;

import java.io.InputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.UUID;

import com.google.inject.Singleton;
import com.yugabyte.yw.commissioner.Common;
import com.yugabyte.yw.common.ApiResponse;
import com.yugabyte.yw.models.PriceComponent;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.fasterxml.jackson.databind.JsonNode;
import com.google.inject.Inject;
import com.yugabyte.yw.common.ApiHelper;
import com.yugabyte.yw.models.InstanceType;
import com.yugabyte.yw.models.InstanceType.InstanceTypeDetails;
import com.yugabyte.yw.models.InstanceType.VolumeType;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;

import play.Environment;
import play.libs.Json;
import play.mvc.Result;

import static play.mvc.Http.Status.INTERNAL_SERVER_ERROR;

// TODO: move pricing data fetch to ybcloud.
@Singleton
public class AWSInitializer extends AbstractInitializer {
  private static final boolean enableVerboseLogging = false;

  private List<Map<String, String>> ec2AvailableInstances = new ArrayList<>();
  private Provider provider;

  @Inject
  Environment environment;

  /**
   * Entry point to initialize AWS. This will create the various InstanceTypes and their
   * corresponding PriceComponents per Region for AWS as well as the EBS pricing info.
   *
   * @param customerUUID UUID of the Customer.
   * @param providerUUID UUID of the Customer's configured AWS.
   * @return A response result that can be returned to the user to indicate success/failure.
   */
  @Override
  public Result initialize(UUID customerUUID, UUID providerUUID) {
    try {
      provider = Provider.get(customerUUID, providerUUID);

      LOG.info("Initializing AWS instance type and pricing info.");
      LOG.info("This operation may take a few minutes...");
      // Get the price Json object stored locally at conf/aws_pricing.
      for (Region region : provider.regions) {
        JsonNode regionJson = null;
        try {
          InputStream regionStream = environment.resourceAsStream("aws_pricing/" + region.code);
          ObjectMapper mapper = new ObjectMapper();
          regionJson = mapper.readTree(regionStream);
        } catch (IOException e) {
          LOG.error("Failed to parse region metadata from region ", region.code);
          return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
        }

        // The products sub-document has the list of EC2 products along with the SKU, its format is:
        //    {
        //      ...
        //      "products" : {
        //        <productDetailsJson, which is a list of product details>
        //      }
        //    }
        JsonNode productDetailsListJson = regionJson.get("products");

        // The "terms" or price details json object has the following format:
        //  "terms" : {
        //    "OnDemand" : {
        //      <onDemandJson, which is a list of price details objects>
        //    }
        //  }
        JsonNode onDemandJson = regionJson.get("terms").get("OnDemand");

        storeEBSPriceComponents(productDetailsListJson, onDemandJson);
        storeInstancePriceComponents(productDetailsListJson, onDemandJson);
        parseProductDetailsList(productDetailsListJson);

        // Create the instance types.
        storeInstanceTypeInfoToDB();
      }
      LOG.info("Successfully finished parsing pricing info.");
    } catch (Exception e) {
      LOG.error("AWS initialize failed", e);
      return ApiResponse.error(INTERNAL_SERVER_ERROR, e.getMessage());
    }

    return ApiResponse.success("AWS Initialized.");
  }

  /**
   * This will store the PriceComponents corresponding to EBS. Example IO1 size json blobs:
   * "KA7RG53ZHMXMZFAF" : {
   *   "KA7RG53ZHMXMZFAF.JRTCKXETXF" : {
   *     "offerTermCode" : "JRTCKXETXF",
   *     "sku" : "KA7RG53ZHMXMZFAF",
   *     "effectiveDate" : "2017-06-01T00:00:00Z",
   *     "priceDimensions" : {
   *       "KA7RG53ZHMXMZFAF.JRTCKXETXF.6YS6EN2CT7" : {
   *         "rateCode" : "KA7RG53ZHMXMZFAF.JRTCKXETXF.6YS6EN2CT7",
   *         "description" : "$0.145 per GB-month of Provisioned IOPS SSD (io1)  provisioned storage - EU (London)",
   *         "beginRange" : "0",
   *         "endRange" : "Inf",
   *         "unit" : "GB-Mo",
   *         "pricePerUnit" : {
   *           "USD" : "0.1450000000"
   *         },
   *         "appliesTo" : [ ]
   *       }
   *     },
   *     "termAttributes" : { }
   *   }
   * },
   * "KA7RG53ZHMXMZFAF" : {
   *   "sku" : "KA7RG53ZHMXMZFAF",
   *   "productFamily" : "Storage",
   *   "attributes" : {
   *     "servicecode" : "AmazonEC2",
   *     "location" : "EU (London)",
   *     "locationType" : "AWS Region",
   *     "storageMedia" : "SSD-backed",
   *     "volumeType" : "Provisioned IOPS",
   *     "maxVolumeSize" : "16 TiB",
   *     "maxIopsvolume" : "20000",
   *     "maxThroughputvolume" : "320 MB/sec",
   *     "usagetype" : "EUW2-EBS:VolumeUsage.piops",
   *     "operation" : ""
   *   }
   * },
   *
   * @param productDetailsListJson Products sub-document with list of EC2 products along with SKU.
   * @param onDemandJson Price details json object.
   */
  private void storeEBSPriceComponents(JsonNode productDetailsListJson, JsonNode onDemandJson) {
    LOG.info("Parsing product details list to store pricing info");
    for (JsonNode productDetailsJson : productDetailsListJson) {
      String sku = productDetailsJson.get("sku").textValue();
      JsonNode attributesJson = productDetailsJson.get("attributes");
      JsonNode regionJson = attributesJson.get("location");
      if (regionJson == null) {
        if (enableVerboseLogging) {
          LOG.error("No region available for product SKU " + sku + ". Skipping.");
        }
        continue;
      }
      Region region = Region.find.query().where()
          .eq("provider_uuid", provider.uuid)
          .eq("name", regionJson.textValue())
          .findOne();
      if (region == null) {
        if (enableVerboseLogging) {
          LOG.error("No region " + regionJson.textValue() + " available");
        }
        continue;
      }
      if (productDetailsJson.get("productFamily") != null) {
        switch (productDetailsJson.get("productFamily").textValue()) {
          case "Storage":
            JsonNode volumeType = attributesJson.get("volumeType");
            if (volumeType.textValue().equals("Provisioned IOPS")) {
              storeEBSPriceComponent(sku, PublicCloudConstants.IO1_SIZE, region, onDemandJson);
            } else if (volumeType.textValue().equals("General Purpose")) {
              storeEBSPriceComponent(sku, PublicCloudConstants.GP2_SIZE, region, onDemandJson);
            }
            break;
          case "System Operation":
            if (attributesJson.get("group").textValue().equals("EBS IOPS")) {
              storeEBSPriceComponent(sku, PublicCloudConstants.IO1_PIOPS, region, onDemandJson);
            }
            break;
          default:
            break;
        }
      }
    }
  }

  /**
   * Given info about a single EBS item (size/piops) in a specific region, store its PriceComponent.
   *
   * @param sku SKU of the EBS item in its region.
   * @param componentCode Code for the EBS item (e.g. io1.size).
   * @param region The region the EBS item is in (e.g. us-west2).
   * @param onDemandJson Price details json object.
   */
  private void storeEBSPriceComponent(String sku, String componentCode, Region region,
                                      JsonNode onDemandJson) {
    // Then create the pricing component object by grabbing the first item (should only have one)
    // and populating the PriceDetails with all the relevant information
    PriceComponent.PriceDetails priceDetails = new PriceComponent.PriceDetails();
    JsonNode product = onDemandJson.get(sku).elements().next();
    JsonNode priceDimensions = product.get("priceDimensions").elements().next();

    // Get the currency & price per unit
    String currency = priceDimensions.get("pricePerUnit").fields().next().getKey();
    String pricePerUnit = priceDimensions.get("pricePerUnit").get(currency).textValue();
    String unit = priceDimensions.get("unit").textValue().toUpperCase();
    if (!(unit.endsWith("-MO") || unit.endsWith("MONTH"))) {
      throw new RuntimeException("Unit is not per month: " + unit);
    }
    priceDetails.currency = PriceComponent.PriceDetails.Currency.valueOf(currency);
    priceDetails.setUnitFromString(unit);
    priceDetails.pricePerUnit = Double.parseDouble(pricePerUnit);
    priceDetails.pricePerMonth = priceDetails.pricePerUnit;
    priceDetails.pricePerDay = priceDetails.pricePerMonth / 30.0;
    priceDetails.pricePerHour = priceDetails.pricePerDay / 24.0;

    // Get everything else
    priceDetails.description = priceDimensions.get("description").textValue();
    priceDetails.effectiveDate = product.get("effectiveDate").textValue();

    // Save to db
    PriceComponent.upsert(provider.code, region.code, componentCode, priceDetails);
  }

  /**
   * This will store the PriceComponent corresponding to the InstanceType itself.
   * Each price detail object has the format:
   *      "DQ578CGN99KG6ECF" : {
   *        "DQ578CGN99KG6ECF.JRTCKXETXF" : {
   *          "offerTermCode" : "JRTCKXETXF",
   *          "sku" : "DQ578CGN99KG6ECF",
   *          "effectiveDate" : "2016-08-01T00:00:00Z",
   *          "priceDimensions" : {
   *            "DQ578CGN99KG6ECF.JRTCKXETXF.6YS6EN2CT7" : {
   *              "rateCode" : "DQ578CGN99KG6ECF.JRTCKXETXF.6YS6EN2CT7",
   *              "description" : "$4.931 per On Demand Windows hs1.8xlarge Instance Hour",
   *              "beginRange" : "0",
   *              "endRange" : "Inf",
   *              "unit" : "Hrs",
   *              "pricePerUnit" : {
   *                "USD" : "4.9310000000"
   *              },
   *              "appliesTo" : [ ]
   *            }
   *          },
   *          "termAttributes" : { }
   *        }
   *      }
   *
   * @param productDetailsListJson Products sub-document with list of EC2 products along with SKU.
   * @param onDemandJson Price details json object.
   */
  private void storeInstancePriceComponents(JsonNode productDetailsListJson,
                                            JsonNode onDemandJson) {

    // Get SKUs associated with Instances
    LOG.info("Parsing product details list to store pricing info");
    for (JsonNode productDetailsJson : productDetailsListJson) {

      Map<String, String> productAttrs = extractAllAttributes(productDetailsJson);
      boolean include = true;

      // Make sure this is a compute instance.
      include &= matches(productAttrs, "productFamily", FilterOp.Equals, "Compute Instance");
      // The service code should be 'AmazonEC2'.
      include &= matches(productAttrs, "servicecode", FilterOp.Equals, "AmazonEC2");
      // Filter by the OS we support.
      include &= (matches(productAttrs, "operatingSystem", FilterOp.Equals, "Linux"));
      // Pick the supported license models.
      include &= (matches(productAttrs, "licenseModel", FilterOp.Equals, "No License required") ||
          matches(productAttrs, "licenseModel", FilterOp.Equals, "NA"));
      // Pick the valid disk drive types.
      include &= (matches(productAttrs, "storage", FilterOp.Contains, "SSD") ||
          matches(productAttrs, "storage", FilterOp.Contains, "EBS"));
      // Make sure it is current generation.
      include &= matches(productAttrs, "currentGeneration", FilterOp.Equals, "Yes");
      // Make sure tenancy is shared.
      include &= matches(productAttrs, "tenancy", FilterOp.Equals, "Shared");
      // Make sure it is the base instance type.
      include &= matches(productAttrs, "preInstalledSw", FilterOp.Equals, "NA");
      // Make sure instance type is supported.
      include &= isInstanceTypeSupported(productAttrs);

      if (include) {
        JsonNode attributesJson = productDetailsJson.get("attributes");
        storeInstancePriceComponent(
            productDetailsJson.get("sku").textValue(),
            attributesJson.get("instanceType").textValue(),
            attributesJson.get("location").textValue(),
            onDemandJson);
      }
    }
  }

  /**
   * Given info about a single AWS InstanceType in a specific region, store its PriceComponent.
   *
   * @param sku SKU of the InstanceType in its region.
   * @param instanceCode Code for the InstanceType (e.g. m3.medium).
   * @param regionName Name for the region the InstanceType is in (e.g. "US West (Oregon)").
   * @param onDemandJson Price details json object.
   */
  private void storeInstancePriceComponent(String sku, String instanceCode, String regionName,
                                           JsonNode onDemandJson) {

    // First check that region exists
    Region region = Region.find.query().where()
      .eq("provider_uuid", provider.uuid)
      .eq("name", regionName)
      .findOne();
    if (region == null) {
      LOG.error("Region " + regionName + " not found. Skipping.");
      return;
    }

    // Then create the pricing component object by grabbing the first item (should only have one)
    // and populating the PriceDetails with all the relevant information
    PriceComponent.PriceDetails priceDetails = new PriceComponent.PriceDetails();
    JsonNode product = onDemandJson.get(sku).elements().next();
    JsonNode priceDimensions = product.get("priceDimensions").elements().next();

    // Get the currency & price per unit
    String currency = priceDimensions.get("pricePerUnit").fields().next().getKey();
    String pricePerUnit = priceDimensions.get("pricePerUnit").get(currency).textValue();
    String unit = priceDimensions.get("unit").textValue().toUpperCase();
    if (!(unit.equals("HRS") || unit.equals("HOURS"))) {
      throw new RuntimeException("Unit is not per hour: " + unit);
    }
    priceDetails.setUnitFromString(unit);
    priceDetails.currency = PriceComponent.PriceDetails.Currency.valueOf(currency);
    priceDetails.pricePerUnit = Double.parseDouble(pricePerUnit);
    priceDetails.pricePerHour = priceDetails.pricePerUnit;
    priceDetails.pricePerDay = priceDetails.pricePerUnit * 24.0;
    priceDetails.pricePerMonth = priceDetails.pricePerDay * 30.0;

    // Get everything else
    priceDetails.description = priceDimensions.get("description").textValue();
    priceDetails.effectiveDate = product.get("effectiveDate").textValue();

    // Save to db
    if (Double.parseDouble(pricePerUnit) != 0.0) {
      PriceComponent.upsert(provider.code, region.code, instanceCode, priceDetails);
    }
  }

  /**
   * Given a JSON blob containing details on various EC2 products, update the ec2AvailableInstances
   * map, which contains information on the various instances available through EC2. Each entry in
   * the product details map looks like:
   * "DQ578CGN99KG6ECF" : {
   *   "sku" : "DQ578CGN99KG6ECF",
   *   "productFamily" : "Compute Instance",
   *   "attributes" : {
   *     "servicecode" : "AmazonEC2",
   *     "location" : "US East (N. Virginia)",
   *     "locationType" : "AWS Region",
   *     "instanceType" : "hs1.8xlarge",
   *     "currentGeneration" : "No",
   *     "instanceFamily" : "Storage optimized",
   *     "vcpu" : "17",
   *     "physicalProcessor" : "Intel Xeon E5-2650",
   *     "clockSpeed" : "2 GHz",
   *     "memory" : "117 GiB",
   *     "storage" : "24 x 2000",
   *     "networkPerformance" : "10 Gigabit",
   *     "processorArchitecture" : "64-bit",
   *     "tenancy" : "Shared",
   *     "operatingSystem" : "Windows",
   *     "licenseModel" : "License Included",
   *     "usagetype" : "BoxUsage:hs1.8xlarge",
   *     "operation" : "RunInstances:0002",
   *     "preInstalledSw" : "NA"
   *   }
   * }
   *
   * @param productDetailsListJson A JSON blob as described above.
   */
  private void parseProductDetailsList(JsonNode productDetailsListJson) {
    LOG.info("Parsing product details list");
    Iterator<JsonNode> productDetailsListIter = productDetailsListJson.elements();
    while (productDetailsListIter.hasNext()) {
      JsonNode productDetailsJson = productDetailsListIter.next();

      Map<String, String> productAttrs = extractAllAttributes(productDetailsJson);

      boolean include = true;

      // Make sure this is a compute instance.
      include &= matches(productAttrs, "productFamily", FilterOp.Equals, "Compute Instance");
      // The service code should be 'AmazonEC2'.
      include &= matches(productAttrs, "servicecode", FilterOp.Equals, "AmazonEC2");
      // Filter by the OS we support.
      include &= (matches(productAttrs, "operatingSystem", FilterOp.Equals, "Linux"));
      // Pick the supported license models.
      include &= (matches(productAttrs, "licenseModel", FilterOp.Equals, "No License required") ||
                  matches(productAttrs, "licenseModel", FilterOp.Equals, "NA"));
      // Pick the valid disk drive types.
      include &= (matches(productAttrs, "storage", FilterOp.Contains, "SSD") ||
                  matches(productAttrs, "storage", FilterOp.Contains, "EBS"));
      // Make sure it is current generation.
      include &= matches(productAttrs, "currentGeneration", FilterOp.Equals, "Yes");
      // Make sure tenancy is shared.
      include &= matches(productAttrs, "tenancy", FilterOp.Equals, "Shared");
      // Make sure it is the base instance type.
      include &= matches(productAttrs, "preInstalledSw", FilterOp.Equals, "NA");
      // Make sure instance type is supported.
      include &= isInstanceTypeSupported(productAttrs);

      if (!include) {
        if (enableVerboseLogging) {
          LOG.info("Skipping product");
        }
        continue;
      }

      if (enableVerboseLogging) {
        LOG.info("Found matching product with sku={}, instanceType={}", productAttrs.get("sku"),
            productAttrs.get("instanceType"));
      }
      ec2AvailableInstances.add(productAttrs);
    }
  }

  /**
   * Build a KVP Map for the attributes that make up a given product in the EC2 products JSON.
   *
   * @param productDetailsJson An entry in the EC2 product details JSON list.
   * @return A KVP Map for the attributes of the provided entry.
   */
  private Map<String, String> extractAllAttributes(JsonNode productDetailsJson) {
    Map<String, String> productAttrs = new HashMap<>();
    productAttrs.put("sku", productDetailsJson.get("sku").textValue());
    productAttrs.put("productFamily", productDetailsJson.get("productFamily") !=  null ?
      productDetailsJson.get("productFamily").textValue() : "");

    // Iterate over all the attributes.
    Iterator<String> iter = productDetailsJson.get("attributes").fieldNames();
    while (iter.hasNext()) {
      String key = iter.next();
      productAttrs.put(key, productDetailsJson.get("attributes").get(key).textValue());
    }
    if (enableVerboseLogging) {
      StringBuilder sb = new StringBuilder();
      sb.append("Product: sku=").append(productDetailsJson.get("sku").textValue());
      sb.append(", productFamily=").append(productDetailsJson.get("productFamily").textValue());
      for (String key : productAttrs.keySet()) {
        sb.append(", ").append(key).append("=").append(productAttrs.get(key));
      }
      LOG.info(sb.toString());
    }
    return productAttrs;
  }

  /**
   * Store information about the various instance types to the database. Uses UPSERT semantics if
   * the row for the instance type already exists.
   */
  private void storeInstanceTypeInfoToDB() {
    LOG.info("Storing AWS instance type and pricing info in Yugaware DB");
    // First reset all the JSON details of all entries in the table, as we are about to refresh it.
    Common.CloudType provider = Common.CloudType.aws;
    InstanceType.resetInstanceTypeDetailsForProvider(provider);
    String instanceTypeCode = null;

    for (Map<String, String> productAttrs : ec2AvailableInstances) {
      // Get the instance type.
      instanceTypeCode = productAttrs.get("instanceType");

      // The number of cores is the number of vcpu's.
      if (productAttrs.get("vcpu") == null) {
        String msg = "Error parsing sku=" + productAttrs.get("sku") + ", num vcpu missing";
        LOG.error(msg);
        throw new RuntimeException(msg);
      }
      Integer numCores = Integer.parseInt(productAttrs.get("vcpu"));

      // Parse the memory size.
      String memSizeStrGB = productAttrs.get("memory")
          .replaceAll("(?i) gib", "")
          .replaceAll(",", "");
      Double memSizeGB = Double.parseDouble(memSizeStrGB);

      Integer volumeCount;
      Integer volumeSizeGB;
      VolumeType volumeType;
      // Parse the local instance store details. Format of the raw data is one of the following:
      // 1 x 75 NVMe SSD
      // EBS only
      // 125 GB NVMe SSD
      // 1 x 800 SSD
      // 12 x 2000 HDD
      // 2 x 900 GB NVMe SSD
      String[] parts = productAttrs.get("storage").replaceAll(",", "").split(" ");
      if (parts.length < 4) {
        if (!productAttrs.get("storage").equals("EBS only")) {
          String msg = "Volume type not specified in product sku=" + productAttrs.get("sku") +
              ", storage={" + productAttrs.get("storage") + "}";
          LOG.error(msg);
          throw new UnsupportedOperationException(msg);
        } else {
          // TODO: hardcode me not?
          volumeCount = 2;
          volumeSizeGB = 250;
          volumeType = VolumeType.EBS;
        }
      } else if (instanceTypeCode.startsWith("i3")) {
        // TODO: remove this once aws pricing api is fixed
        // TODO: see discussion: https://forums.aws.amazon.com/thread.jspa?messageID=783507
        String[] instanceTypeParts = instanceTypeCode.split("\\.");
        volumeType = VolumeType.SSD;
        switch (instanceTypeParts[1]) {
          case "large":
            volumeCount = 1;
            volumeSizeGB = 475;
            break;
          case "xlarge":
            volumeCount = 1;
            volumeSizeGB = 950;
            break;
          case "2xlarge":
            volumeCount = 1;
            volumeSizeGB = 1900;
            break;
          case "4xlarge":
            volumeCount = 2;
            volumeSizeGB = 1900;
            break;
          case "8xlarge":
            volumeCount = 4;
            volumeSizeGB = 1900;
            break;
          case "16xlarge":
          default:
            volumeCount = 8;
            volumeSizeGB = 1900;
            break;
        }
      } else {
        if (parts[1].equals("x")) {
          volumeCount = Integer.parseInt(parts[0]);
          volumeSizeGB = Integer.parseInt(parts[2]);
          if (parts[3].equals("GB")) {
            volumeType = VolumeType.valueOf(parts[4].toUpperCase());
          } else {
            volumeType = VolumeType.valueOf(parts[3].toUpperCase());
          }

        } else {
          volumeCount = 1;
          volumeSizeGB = Integer.parseInt(parts[0]);
          volumeType = VolumeType.valueOf(parts[2].toUpperCase());
        }
      }

      if (enableVerboseLogging) {
        LOG.info("Instance type entry ({}, {}): {} cores, {} GB RAM, {} x {} GB {}", provider.name(),
                 instanceTypeCode, numCores, memSizeGB, volumeCount, volumeSizeGB, volumeType);
      }

      // Create the instance type model. If one already exists, overwrite it.
      InstanceType instanceType = InstanceType.get(provider.name(), instanceTypeCode);
      if (instanceType == null) {
        instanceType = new InstanceType();
      }
      InstanceTypeDetails details = instanceType.instanceTypeDetails;
      if (details == null) {
        details = new InstanceTypeDetails();
      }
      if (details.volumeDetailsList.isEmpty()) {
        details.setVolumeDetailsList(volumeCount, volumeSizeGB, volumeType);
      }
      if (details.tenancy == null) {
        details.tenancy = PublicCloudConstants.Tenancy.Shared;
      }

      // Update the object.
      if (enableVerboseLogging) {
        LOG.debug("Saving {} ({} cores, {}GB) with details {}", instanceType.idKey.toString(),
            instanceType.numCores, instanceType.memSizeGB, Json.stringify(Json.toJson(details)));
      }
      InstanceType.upsert(provider.name(), instanceTypeCode, numCores, memSizeGB, details);
    }
  }

  enum FilterOp {
    Equals,
    Contains,
  }

  private boolean matches(Map<String, String> objAttrs, String name, FilterOp op, String value) {
    switch (op) {
      case Equals:
        return value.equals(objAttrs.get(name));
      case Contains:
        return objAttrs.containsKey(name) && objAttrs.get(name).contains(value);
      default:
        return false;
    }
  }

  private boolean isInstanceTypeSupported(Map<String, String> productAttributes) {
    return InstanceType.AWS_INSTANCE_PREFIXES_SUPPORTED.stream().anyMatch(
      productAttributes.getOrDefault("instanceType", "")::startsWith);
  }
}
