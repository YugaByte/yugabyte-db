// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import PropTypes from 'prop-types';
import { Image, Label } from 'react-bootstrap';
import { RootMarkerIcon, ReadReplicaMarkerIcon, CacheMarkerIcon } from './images';
import './stylesheets/YBMapLegendItem.scss';
import { isNonEmptyArray, isNonEmptyObject } from '../../utils/ObjectUtils';
import { PROVIDER_TYPES } from '../../config';
import pluralize from 'pluralize';

export default class YBMapLegendItem extends Component {
  render() {
    const { regions, provider, providerType, title, type } = this.props;
    let legendItemIcon = '';
    let legendText = '';
    if (type === 'Root') {
      legendItemIcon = <Image src={RootMarkerIcon} className="legend-img" />;
    } else if (type === 'Cache') {
      legendItemIcon = <Image src={CacheMarkerIcon} className="legend-img" />;
    } else if (type === 'ReadReplica') {
      legendItemIcon = <Image src={ReadReplicaMarkerIcon} className="legend-img" />;
    } else if (type === 'Region') {
      legendItemIcon = <div className="marker-cluster-small provider-marker-cluster">#</div>;
    }
    let regionInfo = <span />;
    if (type !== 'Region') {
      if (isNonEmptyArray(regions)) {
        const legendSubTexts = [pluralize('Region', regions.length, true)];
        if (isNonEmptyObject(provider)) {
          const providerInfo = PROVIDER_TYPES.find(
            (providerType) => providerType.code === provider.code
          );
          legendText = providerInfo.label;
        }
        const azList = regions.reduce((zones, region) => zones.concat(region.azList), []);
        const nodeCount = azList.reduce((nodeCnt, az) => (nodeCnt += az.numNodesInAZ), 0);

        if (azList.length > 0) {
          legendSubTexts.push(pluralize('AZ', azList.length, true));
        }

        if (nodeCount > 0) {
          legendSubTexts.push(
            pluralize(providerType === 'kubernetes' ? 'Pod' : 'Node', nodeCount, true)
          );
        }

        regionInfo = (
          <Fragment>
            <Label bsStyle="primary">{legendText}</Label>
            <p>{legendSubTexts.join(', ')}</p>
          </Fragment>
        );
      } else {
        regionInfo = <Fragment>{'None'}</Fragment>;
      }
    }
    return (
      <div className="map-legend-item">
        <div className="icon-hang-left">{legendItemIcon}</div>
        <h5 className="map-legend-heading">{title}</h5>
        <div className="map-legend-text">{regionInfo}</div>
      </div>
    );
  }
}

YBMapLegendItem.defaultProps = {
  type: 'Region',
  regions: [],
  provider: {}
};

YBMapLegendItem.propTypes = {
  title: PropTypes.string.isRequired,
  type: PropTypes.oneOf(['Root', 'ReadReplica', 'Cache', 'Region']),
  regions: PropTypes.array,
  providerCode: PropTypes.object
};
