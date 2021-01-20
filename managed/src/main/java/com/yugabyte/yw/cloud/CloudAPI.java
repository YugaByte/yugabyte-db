package com.yugabyte.yw.cloud;

import com.yugabyte.yw.models.Provider;
import com.yugabyte.yw.models.Region;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import javax.inject.Inject;
import javax.inject.Singleton;
import java.util.Map;
import java.util.Set;

public interface CloudAPI {

  @Singleton
  class Factory {
    Logger LOG = LoggerFactory.getLogger(CloudAPI.class);

    private final Map<String, CloudAPI> cloudAPIMap;

    @Inject
    public Factory(Map<String, CloudAPI> cloudAPIMap) {
      this.cloudAPIMap = cloudAPIMap;
      LOG.info("Created cloud API factory for {}", cloudAPIMap.keySet());
    }

    public CloudAPI get(String code) {
      return cloudAPIMap.get(code);
    }
  }

  /**
   * Check instance offerings by making cloud call for all the regions in azByRegionMap.keySet().
   * Use supplied instanceTypesFilter and availabilityZones (azByRegionMap) as filter for this
   * describe call.
   *
   * @param provider            the cloud provider bean for the AWS provider.
   * @param azByRegionMap       user selected availabilityZone codes by their parent region.
   * @param instanceTypesFilter list of instanceTypes we want to list the offerings for
   * @return a map. Key of this map is instance type like "c5.xlarge" and value is all the
   * availabilityZones for which the instance type is being offered.
   */
  Map<String, Set<String>> offeredZonesByInstanceType(
    Provider provider,
    Map<Region, Set<String>> azByRegionMap,
    Set<String> instanceTypesFilter);
}
