// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { isNonEmptyObject } from '../../../utils/ObjectUtils';
const statusTypes = {
  singleRF: {
    currentStatusIcon: 'fa fa-exclamation',
    currentStatusString:
      'Primary data placement is not redundant, universe cannot survive even 1 node failure',
    currentStatusClass: 'yb-warn-color'
  },
  azWarning: {
    currentStatusIcon: 'fa fa-exclamation',
    currentStatusString:
      'Primary data placement is not geo-redundant,' +
      ' universe cannot survive even 1 availability zone failure',
    currentStatusClass: 'yb-warn-color'
  },
  regionWarning: {
    currentStatusIcon: 'fa fa-check',
    currentStatusString:
      'Primary data placement is geo-redundant,' +
      ' universe can survive at least 1 availability zone failure',
    currentStatusClass: 'yb-success-color'
  },
  multiRegion: {
    currentStatusIcon: 'fa fa-check',
    currentStatusString:
      'Primary data placement is fully geo-redundant,' +
      ' universe can survive at least 1 region failure',
    currentStatusClass: 'yb-success-color'
  },
  notEnoughNodesConfigured: {
    currentStatusIcon: 'fa fa-times',
    currentStatusString: 'Not Enough Nodes Configured',
    currentStatusClass: 'yb-fail-color'
  },
  notEnoughNodes: {
    currentStatusIcon: 'fa fa-times',
    currentStatusString: 'Not Enough Nodes',
    currentStatusClass: 'yb-fail-color'
  },
  noFieldsChanged: {
    currentStatusIcon: 'fa fa-times',
    currentStatusString: 'At Least One Field Must Be Modified',
    currentStatusClass: 'yb-fail-color'
  }
};

export default class AZPlacementInfo extends Component {
  static propTypes = {
    placementInfo: PropTypes.object.isRequired
  };
  render() {
    const { placementInfo, placementCloud, providerCode } = this.props;
    let currentStatusType;

    // If placementInfo and placementCloud is empty and provier is not onprem then return
    if ((!isNonEmptyObject(placementInfo) || !isNonEmptyObject(placementCloud)) && providerCode !== "onprem") {
      return <span />;
    }

    const replicationFactor = placementInfo.replicationFactor;
    const regionList = placementCloud?.regionList || [];
    let multiRegion = true;
    let multiAz = true;

    // This logic is to help determine whether any AZ or region in the current universe config
    // contains a majority of tablet replicas. This determines whether the current config will
    // result in a cluster that is resilient to AZ or Region level failures while still maintaining
    // quorum.
    regionList.forEach((region) => {
      const azList = region.azList;
      let regionNumReplicas = 0;
      azList.forEach((az) => {
        regionNumReplicas += az.replicationFactor;
        if (replicationFactor % 2 === 0) {
          if (replicationFactor / 2 < az.replicationFactor) {
            multiAz = false;
          }
        } else {
          if ((replicationFactor - 1) / 2 < az.replicationFactor) {
            multiAz = false;
          }
        }
      });

      if (replicationFactor % 2 === 0) {
        if (replicationFactor / 2 < regionNumReplicas) {
          multiRegion = false;
        }
      } else {
        if ((replicationFactor - 1) / 2 < regionNumReplicas) {
          multiRegion = false;
        }
      }
    });
    

    if (placementInfo.error) {
      currentStatusType = placementInfo.error.type;
    } else if (replicationFactor === 1) {
      currentStatusType = 'singleRF';
    } else if (multiRegion) {
      currentStatusType = 'multiRegion';
    } else if (multiAz) {
      currentStatusType = 'regionWarning';
    } else {
      currentStatusType = 'azWarning';
    }

    return (
      <div>
        <span className={statusTypes[currentStatusType].currentStatusClass}>
          &nbsp;
          <i className={statusTypes[currentStatusType].currentStatusIcon} />
          &nbsp;{statusTypes[currentStatusType].currentStatusString}
        </span>
      </div>
    );
  }
}
