// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { RegionMap } from '../../maps';
import { RegionMapLegend } from '../../maps';
import { isNonEmptyArray, isValidObject, isEmptyArray } from '../../../utils/ObjectUtils';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { getPrimaryCluster, getProviderMetadata } from '../../../utils/UniverseUtils';

export default class UniverseRegionLocationPanel extends Component {
  constructor(props) {
    super(props);
    this.state = { selectedProviders: [] };
  }

  onProviderSelect = (selectedProviders) => {
    this.setState({ selectedProviders: selectedProviders.map((provider) => provider.code) });
  };

  componentDidUpdate(prevProps) {
    if (
      isNonEmptyArray(this.props.cloud.providers.data) &&
      (prevProps.cloud.providers !== this.props.cloud.providers ||
        isEmptyArray(this.state.selectedProviders))
    ) {
      this.setState({
        selectedProviders: this.props.cloud.providers.data.map((provider) => provider.code)
      });
    }
  }

  render() {
    const {
      cloud,
      universe: { universeList },
      cloud: { providers }
    } = this.props;
    const self = this;
    if (
      getPromiseState(providers).isEmpty() ||
      !getPromiseState(cloud.supportedRegionList).isSuccess()
    ) {
      return <span />;
    }

    const completeRegionList = cloud.supportedRegionList.data.filter((region) =>
      self.state.selectedProviders.includes(region.provider.code)
    );
    const universeListByRegions = {};
    if (getPromiseState(universeList).isSuccess()) {
      universeList.data.forEach(function (universeItem) {
        const universePrimaryRegions = getPrimaryCluster(universeItem.universeDetails.clusters)
          .regions;
        if (isNonEmptyArray(universePrimaryRegions)) {
          universePrimaryRegions.forEach(function (regionItem) {
            if (isValidObject(regionItem.uuid)) {
              if (universeListByRegions.hasOwnProperty(regionItem.uuid)) {
                universeListByRegions[regionItem.uuid].push(universeItem);
              } else {
                universeListByRegions[regionItem.uuid] = [universeItem];
              }
            }
          });
        }
      });
    }
    completeRegionList.forEach(function (completeRegionItem, crIdx) {
      delete completeRegionList[crIdx].universes;
      Object.keys(universeListByRegions).forEach(function (regionKey) {
        if (regionKey === completeRegionItem.uuid) {
          completeRegionList[crIdx].universes = universeListByRegions[regionKey];
        }
      });
    });
    const completeProviderList = cloud.providers.data.map((provider) => {
      return getProviderMetadata(provider);
    });

    return (
      <div>
        <RegionMap title="All Supported Regions" regions={completeRegionList} type="All" />
        {isNonEmptyArray(completeProviderList) && (
          <RegionMapLegend
            providers={completeProviderList}
            onProviderSelect={this.onProviderSelect}
          />
        )}
      </div>
    );
  }
}
