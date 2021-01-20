// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { YBMapLegendItem } from '.';
import PropTypes from 'prop-types';
import {
  getPrimaryCluster,
  getReadOnlyCluster,
  getPlacementRegions,
  getPlacementCloud
} from '../../utils/UniverseUtils';

export default class YBMapLegend extends Component {
  static defaultProps = {
    type: 'Region',
    clusters: []
  };

  static propTypes = {
    clusters: PropTypes.array
  };

  render() {
    const { type, clusters } = this.props;
    let mapLegendItems = <span />;
    if (type === 'Universe') {
      const primaryCluster = getPrimaryCluster(clusters);
      const primaryCloud = getPlacementCloud(primaryCluster);
      const readreplicaCluster = getReadOnlyCluster(clusters);
      const readreplicaCloud = getPlacementCloud(readreplicaCluster);

      mapLegendItems = (
        <span>
          <YBMapLegendItem
            providerType={primaryCluster && primaryCluster.userIntent.providerType}
            regions={getPlacementRegions(primaryCluster)}
            provider={primaryCloud}
            title={'Primary Data'}
            type="Root"
          />
          <YBMapLegendItem
            providerType={readreplicaCluster && readreplicaCluster.userIntent.providerType}
            regions={getPlacementRegions(readreplicaCluster)}
            provider={readreplicaCloud}
            title={'Read Replica'}
            type="ReadReplica"
          />
        </span>
      );
    } else if (type === 'Region') {
      mapLegendItems = (
        <span>
          <YBMapLegendItem title={'Regions & Availability Zones'} type="Region" />
        </span>
      );
    }
    return (
      <div className="yb-map-legend">
        {this.props.title && <h4>{this.props.title}</h4>}
        {mapLegendItems}
      </div>
    );
  }
}
