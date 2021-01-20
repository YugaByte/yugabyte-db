// Copyright (c) YugaByte, Inc.

import { getClusterByType } from './UniverseUtils';
import _ from 'lodash';

export function isDefinedNotNull(obj) {
  return typeof obj !== 'undefined' && obj !== null;
}

export function isEmptyArray(arr) {
  return _.isArray(arr) && arr.length === 0;
}

export function isNonEmptyArray(arr) {
  return _.isArray(arr) && arr.length > 0;
}

export function isNullOrEmpty(obj) {
  if (obj == null) {
    return true;
  }
  return _.isObject(obj) && Object.keys(obj).length === 0;
}

export function isEmptyObject(obj) {
  if (typeof obj === 'undefined') {
    return true;
  }
  return _.isObject(obj) && Object.keys(obj).length === 0;
}

export function isNonEmptyObject(obj) {
  return _.isObject(obj) && Object.keys(obj).length > 0;
}

export function isNonEmptyString(str) {
  return _.isString(str) && str.trim().length > 0;
}

export function isEmptyString(str) {
  return _.isString(str) && str.trim().length === 0;
}

export function removeNullProperties(obj) {
  for (const propName in obj) {
    if (obj[propName] === null || obj[propName] === undefined) {
      delete obj[propName];
    }
  }
}

// TODO: Move functions below to ArrayUtils.js?

export function sortByLengthOfArrayProperty(array, propertyName) {
  function arrayLengthComparator(item) {
    return item[propertyName] ? item[propertyName].length : 0;
  }
  return _.sortBy(array, arrayLengthComparator);
}

export function groupWithCounts(array) {
  const counts = {};
  array.forEach(function (item) {
    counts[item] = counts[item] || 0;
    counts[item]++;
  });
  return counts;
}

export function sortedGroupCounts(array) {
  const counts = groupWithCounts(array);
  return Object.keys(counts)
    .sort()
    .map(function (item) {
      return {
        value: item,
        count: counts[item]
      };
    });
}

export function pickArray(objects, propertyNames) {
  return _.map(objects, _.partialRight(_.pick, propertyNames));
}

// TODO: Move these functions to Universe and UserIntent model/class files.

export function areIntentsEqual(userIntent1, userIntent2) {
  return (
    isDefinedNotNull(userIntent1) &&
    isDefinedNotNull(userIntent2) &&
    _.isEqual(userIntent1.numNodes, userIntent2.numNodes) &&
    _.isEqual(userIntent1.regionList.sort(), userIntent2.regionList.sort()) &&
    _.isEqual(userIntent1.deviceInfo, userIntent2.deviceInfo) &&
    _.isEqual(userIntent1.replicationFactor, userIntent2.replicationFactor) &&
    _.isEqual(userIntent1.provider, userIntent2.provider) &&
    _.isEqual(userIntent1.universeName, userIntent2.universeName) &&
    _.isEqual(userIntent1.ybSoftwareVersion, userIntent2.ybSoftwareVersion) &&
    _.isEqual(userIntent1.accessKeyCode, userIntent2.accessKeyCode) &&
    _.isEqual(userIntent1.instanceType, userIntent2.instanceType) &&
    _.isEqual(userIntent1.gflags, userIntent2.gflags)
  );
}

// Helper method to check if AZ objects equal
function areAZObjectsEqual(az1, az2) {
  return (
    az1.name === az2.name &&
    az1.numNodesInAZ === az2.numNodesInAZ &&
    az1.isAffinitized === az2.isAffinitized
  );
}

// Helper methods to check if region objects equal
function areRegionObjectsEqual(region1, region2) {
  if (region1.code !== region2.code || region1.name !== region2.name) {
    return false;
  }
  for (let az1Idx = 0; az1Idx < region1.azList.length; az1Idx++) {
    let azFound = false;
    for (let az2Idx = 0; az2Idx < region2.azList.length; az2Idx++) {
      if (areAZObjectsEqual(region1.azList[az1Idx], region2.azList[az2Idx])) {
        azFound = true;
      }
    }
    if (!azFound) {
      return false;
    }
  }
  return true;
}

// Helper method to check if provider objects equal
function areProviderObjectsEqual(provider1, provider2) {
  if (provider1.code !== provider2.code) {
    return false;
  }
  for (let region1Idx = 0; region1Idx < provider1.regionList.length; region1Idx++) {
    let providerFound = false;
    for (let region2Idx = 0; region2Idx < provider2.regionList.length; region2Idx++) {
      if (
        areRegionObjectsEqual(provider1.regionList[region1Idx], provider2.regionList[region2Idx])
      ) {
        providerFound = true;
      }
    }
    if (!providerFound) {
      return false;
    }
  }
  return true;
}

// Helper method to traverse through the placement info objects checking for equality
export function arePlacementInfoEqual(placementInfo1, placementInfo2) {
  for (let cloud1Idx = 0; cloud1Idx < placementInfo1.cloudList.length; cloud1Idx++) {
    let cloudFound = false;
    for (let cloud2Idx = 0; cloud2Idx < placementInfo2.cloudList.length; cloud2Idx++) {
      if (
        areProviderObjectsEqual(
          placementInfo1.cloudList[cloud1Idx],
          placementInfo2.cloudList[cloud2Idx]
        )
      ) {
        cloudFound = true;
      }
    }
    if (!cloudFound) {
      return false;
    }
  }
  return true;
}

export function areUniverseConfigsEqual(config1, config2) {
  const type = isNonEmptyObject(config1) && config1.currentClusterType;
  if (!type) {
    return false;
  }
  const cluster1 = config1 && getClusterByType(config1.clusters, type);
  const cluster2 = config2 && getClusterByType(config2.clusters, type);
  const clustersEqual =
    (isNonEmptyObject(cluster1) && isNonEmptyObject(cluster2)) ||
    (!isNonEmptyObject(cluster1) && !isNonEmptyObject(cluster2));
  let userIntentsEqual = true;
  let placementObjectsEqual = true;
  if (isNonEmptyObject(cluster1) && isNonEmptyObject(cluster2)) {
    if (cluster1.userIntent && cluster2.userIntent) {
      userIntentsEqual = areIntentsEqual(cluster1.userIntent, cluster2.userIntent);
    } else {
      userIntentsEqual = _.isEqual(cluster1.userIntent, cluster2.userIntent);
    }
    if (isNonEmptyObject(cluster1.placementInfo) && isNonEmptyObject(cluster2.placementInfo)) {
      placementObjectsEqual = arePlacementInfoEqual(cluster1.placementInfo, cluster2.placementInfo);
    } else {
      placementObjectsEqual = _.isEqual(cluster1.placementInfo, cluster2.placementInfo);
    }
  }
  return clustersEqual && userIntentsEqual && placementObjectsEqual;
}

// TODO: Move this function to NumberUtils.js?

export function normalizeToPositiveInt(value) {
  return parseInt(Math.abs(value), 10) || 0;
}

export function normalizeToValidPort(value) {
  return parseInt(Math.abs(value), 10) || 1;
}

// Provided a String, return the corresponding positive float value. If invalid value, return 0.00.
export function normalizeToPositiveFloat(value) {
  // null -> "0.00"
  if (!_.isString(value)) {
    return '0.00';
  }
  // "1.2.3" -> ["1", "2"]; "a.b" -> ["0", "0"]; "." -> ["0", "0"]; "a" -> ["0"]; "" -> ["0"]
  const splitValue = value
    .split('.')
    .slice(0, 2)
    .map((item) => {
      if (item.length === 0 || isNaN(Math.abs(item))) {
        return '0';
      }
      return item;
    });
  // ["1"] -> ["1", "00"]; ["1", "0"] -> ["1", "00"]
  if (splitValue.length === 1 || splitValue[1] === '0') {
    splitValue[1] = '00';
  }
  // ["-5", "1"] -> ["5", "1"]; ["0005", "1"] -> ["5", "1"]
  splitValue[0] = Math.abs(splitValue[0]).toString(10);
  // ["1", "2"] -> "1.2"
  return splitValue.join('.');
}

// TODO: Move the functions below to StringUtils.js?

export function trimString(string) {
  return string && string.trim();
}

export function convertSpaceToDash(string) {
  return string && string.replace(/\s+/g, '-');
}

export function trimSpecialChars(string) {
  return string && string.replace(/[^a-zA-Z0-9/-]+/g, '');
}

export function sortInstanceTypeList(instanceTypeArr) {
  return instanceTypeArr.sort(function (a, b) {
    return a.instanceTypeCode.localeCompare(b.instanceTypeCode, undefined, {
      numeric: true,
      sensitivity: 'base'
    });
  });
}

export function insertSpacesFromCamelCase(string) {
  string = string.replace(/([a-z])([A-Z])/g, '$1 $2');
  string = string.replace(/([A-Z])([A-Z][a-z])/g, '$1 $2');
  return string;
}

// Official Version string is x.x.x.x-bx
export function sortVersionStrings(arr) {
  const regExp = /^(\d+).(\d+).(\d+).(\d+)(?:-[a-z]+)?(\d+)?/;
  const matchedVersions = arr.filter((a) => a.match(regExp));
  const abnormalVersions = arr.filter((a) => !a.match(regExp));
  return matchedVersions
    .sort((a, b) => {
      const a_arr = a.split(regExp).filter(Boolean);
      const b_arr = b.split(regExp).filter(Boolean);
      for (let idx = 0; idx < a_arr.length; idx++) {
        if (a_arr[idx] !== b_arr[idx]) {
          return parseInt(b_arr[idx], 10) - parseInt(a_arr[idx], 10);
        }
      }
      return 0;
    })
    .concat(abnormalVersions.sort((a, b) => a.localeCompare(b)));
}

export function getPointsOnCircle(numPoints, center, radius) {
  const x0 = center[0];
  const y0 = center[1];
  const pointsOnCircle = [];
  for (let i = 0; i < numPoints; i++) {
    const x = x0 + radius * Math.cos((2 * Math.PI * i) / numPoints);
    const y = y0 + radius * Math.sin((2 * Math.PI * i) / numPoints);
    pointsOnCircle.push([x, y]);
  }
  return pointsOnCircle;
}

export function isYAxisGreaterThanThousand(dataArray) {
  for (let counter = 0; counter < dataArray.length; counter++) {
    if (isNonEmptyArray(dataArray[counter].y)) {
      for (let idx = 0; idx < dataArray[counter].y.length; idx++) {
        if (Number(dataArray[counter].y[idx]) > 1000) {
          return true;
        }
      }
    }
  }
  return false;
}

export function divideYAxisByThousand(dataArray) {
  for (let counter = 0; counter < dataArray.length; counter++) {
    if (isNonEmptyArray(dataArray[counter].y)) {
      for (let idx = 0; idx < dataArray[counter].y.length; idx++) {
        dataArray[counter].y[idx] = Number(dataArray[counter].y[idx]) / 1000;
      }
    }
  }
  return dataArray;
}

// FIXME: Deprecated. Change all references to use isNonEmptyArray instead.
export const isValidArray = isNonEmptyArray;

// FIXME: isValidObject has never properly checked the object type.
// FIXME: We have renamed isValidObject to isDefinedNotNull, and
// FIXME: this alias is only kept here for backward compatibility
// FIXME: and should be removed after changing all existing uses.
export const isValidObject = isDefinedNotNull;
