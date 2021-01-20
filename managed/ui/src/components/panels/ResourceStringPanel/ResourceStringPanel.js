// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { DescriptionList } from '../../common/descriptors';
import { getPrimaryCluster, getReadOnlyCluster } from '../../../utils/UniverseUtils';
import { isNonEmptyArray, isNonEmptyObject } from '../../../utils/ObjectUtils';

export default class ResourceStringPanel extends Component {
  static propTypes = {
    type: PropTypes.oneOf(['primary', 'read-replica']).isRequired
  };

  render() {
    const {
      type,
      universeInfo: {
        universeDetails: { clusters }
      },
      providers
    } = this.props;
    let cluster = null;
    if (type === 'primary') {
      cluster = getPrimaryCluster(clusters);
    } else if (type === 'read-replica') {
      cluster = getReadOnlyCluster(clusters);
    }
    const userIntent = cluster && cluster.userIntent;
    let provider = null;
    if (isNonEmptyObject(userIntent) && isNonEmptyArray(providers.data)) {
      if (userIntent.provider) {
        provider = providers.data.find((item) => item.uuid === userIntent.provider);
      } else {
        provider = providers.data.find((item) => item.code === userIntent.providerType);
      }
    }
    const regionList = cluster.regions && cluster.regions.map((region) => region.name).join(', ');
    const connectStringPanelItems = [
      { name: 'Provider', data: provider && provider.name },
      { name: 'Regions', data: regionList },
      { name: 'Instance Type', data: userIntent && userIntent.instanceType },
      { name: 'Replication Factor', data: userIntent.replicationFactor },
      { name: 'SSH Key', data: userIntent.accessKeyCode }
    ];
    return <DescriptionList listItems={connectStringPanelItems} />;
  }
}
