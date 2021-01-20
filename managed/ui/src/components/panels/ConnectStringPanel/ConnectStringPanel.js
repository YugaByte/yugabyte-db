// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { DescriptionList } from '../../common/descriptors';
import { ROOT_URL } from '../../../config';
import './connectStringPanel.css';
import { getPrimaryCluster } from '../../../utils/UniverseUtils';
import { isDefinedNotNull } from '../../../utils/ObjectUtils';

export default class ConnectStringPanel extends Component {
  render() {
    const {
      universeInfo,
      customerId,
      universeInfo: {
        universeDetails: { clusters }
      }
    } = this.props;
    const universeId = universeInfo.universeUUID;
    const endpointUrl =
      ROOT_URL + '/customers/' + customerId + '/universes/' + universeId + '/masters';
    const endpoint = (
      <a href={endpointUrl} target="_blank" rel="noopener noreferrer">
        Endpoint &nbsp;
        <i className="fa fa-external-link" aria-hidden="true"></i>
      </a>
    );
    let version = 'n/a';
    const primaryCluster = getPrimaryCluster(clusters);
    if (
      isDefinedNotNull(primaryCluster) &&
      isDefinedNotNull(primaryCluster.userIntent) &&
      isDefinedNotNull(primaryCluster.userIntent.ybSoftwareVersion)
    ) {
      version = primaryCluster.userIntent.ybSoftwareVersion;
    }
    const connectStringPanelItems = [
      { name: 'Meta Masters', data: endpoint },
      { name: 'DB Version', data: version }
    ];
    return <DescriptionList listItems={connectStringPanelItems} />;
  }
}
