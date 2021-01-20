package com.yugabyte.yw.cloud.aws;

import com.amazonaws.auth.*;
import com.amazonaws.services.ec2.AmazonEC2;
import com.amazonaws.services.ec2.AmazonEC2ClientBuilder;
import com.amazonaws.services.ec2.model.*;
import com.google.common.base.Strings;
import com.yugabyte.yw.cloud.CloudAPI;
import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.stream.Collectors;

import static java.util.stream.Collectors.*;

//TODO - Better handling of UnauthorizedOperation. Ideally we should trigger alert so that
// site admin knows about it
class AWSCloudImpl implements CloudAPI {

  // TODO use aws sdk 2.x and switch to async
  public AmazonEC2 getEcC2Client(Provider provider, Region r) {
    Map<String, String> config = provider.getConfig();
    AWSCredentialsProvider credentialsProvider = getCredsOrFallbackToDefault(
      config.get("AWS_ACCESS_KEY_ID"),
      config.get("AWS_SECRET_ACCESS_KEY"));
    return AmazonEC2ClientBuilder.standard()
      .withRegion(r.code)
      .withCredentials(credentialsProvider)
      .build();
  }

  // TODO: move to some common utils
  private static AWSCredentialsProvider getCredsOrFallbackToDefault(String accessKeyId,
                                                                    String secretAccessKey) {
    if (!Strings.isNullOrEmpty(accessKeyId) && !Strings.isNullOrEmpty(secretAccessKey)) {
      return new AWSStaticCredentialsProvider(
        new BasicAWSCredentials(accessKeyId, secretAccessKey));
    } else {

      // If database creds do not exist we will fallback use default chain.
      return new DefaultAWSCredentialsProviderChain();
    }
  }

  /**
   * Make describe instance offerings calls for all the regions in azByRegionMap.keySet().
   * Use supplied instanceTypesFilter and availabilityZones (azByRegionMap) as filter for this
   * describe call.
   *
   * @param provider            the cloud provider bean for the AWS provider.
   * @param azByRegionMap       user selected availabilityZones by their parent region.
   * @param instanceTypesFilter list of instanceTypes we want to list the offerings for
   * @return a map. Key of this map is instance type like "c5.xlarge" and value is all the
   * availabilityZones for which the instance type is being offered.
   */
  @Override
  public Map<String, Set<String>> offeredZonesByInstanceType(
    Provider provider,
    Map<Region, Set<String>> azByRegionMap,
    Set<String> instanceTypesFilter) {
    Filter instanceTypeFilter = new Filter()
      .withName("instance-type")
      .withValues(instanceTypesFilter);
    // TODO: get rid of parallelStream in favour of async api using aws sdk 2.x
    List<DescribeInstanceTypeOfferingsResult> results =
      azByRegionMap.entrySet().parallelStream()
        .map(regionAZListEntry -> {
          Filter locationFilter = new Filter()
            .withName("location")
            .withValues(regionAZListEntry.getValue());
          return getEcC2Client(provider, regionAZListEntry.getKey())
            .describeInstanceTypeOfferings(new DescribeInstanceTypeOfferingsRequest()
              .withLocationType(LocationType.AvailabilityZone)
              .withFilters(locationFilter, instanceTypeFilter));
        }).collect(Collectors.toList());

    return results.stream().flatMap(result -> result.getInstanceTypeOfferings().stream())
      .collect(groupingBy(InstanceTypeOffering::getInstanceType,
        mapping(InstanceTypeOffering::getLocation, toSet())));
  }
}
