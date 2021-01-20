// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models.helpers;

import com.fasterxml.jackson.annotation.JsonIgnoreProperties;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * The placement info is a tree. The first level contains a list of clouds. Every cloud contains a
 * list of regions. Each region has a list of AZs. The number of leaves in this tree should be
 * equal to the replication factor, and each leaf defines the data placement (by virtue of its
 * path from the first level).
 */
public class PlacementInfo {
  public static class PlacementCloud {
    // The cloud provider id.
    public UUID uuid;
    // The cloud provider code.
    public String code;
    // The list of region in this cloud we want to place data in.
    public List<PlacementRegion> regionList = new ArrayList<PlacementRegion>();
    @Override
    public String toString() {
      String ret = "Cloud=" + code + " ";
      for (PlacementRegion region : regionList) {
        ret += region;
      }
      return ret;
    }
  }

  public static class PlacementRegion {
    // The region provider id.
    public UUID uuid;
    // The actual provider given region code.
    public String code;
    // The region name.
    public String name;
    // The list of AZs inside this region into which we want to place data.
    public List<PlacementAZ> azList = new ArrayList<PlacementAZ>();
    @Override
    public String toString() {
      String ret = "Region=" + code + " : ";
      for (PlacementAZ az : azList) {
        ret += az;
      }
      return ret;
    }
  }

  @JsonIgnoreProperties(ignoreUnknown = true)
  public static class PlacementAZ {
    // The AZ provider id.
    public UUID uuid;
    // The AZ name.
    public String name;
    // The minimum number of copies of data we should place into this AZ.
    public int replicationFactor;
    // The subnet in the AZ.
    public String subnet;
    // Number of nodes in each Az.
    public int numNodesInAZ;
    // Is this an affinitized zone.
    public boolean isAffinitized;
    @Override
    public String toString() {
      return "(AZ=" + name + ", count=" + numNodesInAZ + ", replication factor=" + replicationFactor + ")";
    }
  }

  // The list of clouds to place data in.
  public List<PlacementCloud> cloudList = new ArrayList<PlacementCloud>();

  @Override
  public String toString() {
    String ret = "";
    for (PlacementCloud cloud : cloudList) {
      ret += cloud;
    }
    return ret;
  }
}
