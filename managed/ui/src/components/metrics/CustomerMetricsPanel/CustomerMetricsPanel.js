// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { GraphPanelHeaderContainer, GraphPanelContainer } from '../../metrics';
import PropTypes from 'prop-types';
import { PanelGroup } from 'react-bootstrap';
import { browserHistory } from 'react-router';
import { showOrRedirect } from '../../../utils/LayoutUtils';

const graphPanelTypes = {
  universe: {
    data: [
      'ysql_ops',
      'ycql_ops',
      'yedis_ops',
      'container',
      'server',
      'cql',
      'redis',
      'tserver',
      'master',
      'lsmdb'
    ],
    isOpen: [true, true, false, false, false, false, false, false, false]
  },
  customer: {
    data: [
      'ysql_ops',
      'ycql_ops',
      'yedis_ops',
      'container',
      'server',
      'cql',
      'redis',
      'tserver',
      'master',
      'lsmdb'
    ],
    isOpen: [true, true, false, false, false, false, false, false, false]
  },
  table: {
    data: ['lsmdb_table', 'tserver_table'],
    isOpen: [true, true]
  }
};

/**
 * Move this logic out of render function because we need `selectedUniverse` prop
 * that gets passed down from the `GraphPanelHeader` component.
 */
const PanelBody = ({ origin, selectedUniverse, nodePrefixes, width, tableName }) => {
  const location = browserHistory.getCurrentLocation();
  const currentQuery = location.query;
  return (
    <PanelGroup id={origin + ' metrics'}>
      {graphPanelTypes[origin].data.map(function (type, idx) {
        // if we have subtab query param, then we would have that metric tab open by default
        const isOpen = currentQuery.subtab
          ? type === currentQuery.subtab
          : graphPanelTypes[origin].isOpen[idx];
        return (
          <GraphPanelContainer
            key={idx}
            isOpen={isOpen}
            type={type}
            width={width}
            nodePrefixes={nodePrefixes}
            tableName={tableName}
            selectedUniverse={selectedUniverse}
          />
        );
      })}
    </PanelGroup>
  );
};

export default class CustomerMetricsPanel extends Component {
  static propTypes = {
    origin: PropTypes.oneOf(['customer', 'universe', 'table']).isRequired,
    nodePrefixes: PropTypes.array,
    width: PropTypes.number,
    tableName: PropTypes.string
  };

  static defaultProps = {
    nodePrefixes: [],
    tableName: null,
    width: null
  };

  componentDidMount() {
    const {
      customer: { currentCustomer }
    } = this.props;
    showOrRedirect(currentCustomer.data.features, 'menu.metrics');
  }

  render() {
    const { origin } = this.props;
    return (
      <GraphPanelHeaderContainer origin={origin}>
        <PanelBody {...this.props} />
      </GraphPanelHeaderContainer>
    );
  }
}
