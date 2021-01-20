// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { isValidObject } from '../../../utils/ObjectUtils';
import { DescriptionList } from '../../common/descriptors';

export default class TableInfoPanel extends Component {
  static propTypes = {
    tableInfo: PropTypes.object.isRequired
  };

  render() {
    const { tableInfo } = this.props;
    const tableInfoItems = [
      {
        name: 'Table Name',
        data: isValidObject(tableInfo.tableDetails) ? tableInfo.tableDetails.tableName : ''
      },
      { name: 'Table Type', data: tableInfo.tableType },
      { name: 'Table UUID', data: tableInfo.tableUUID }
    ];
    // Show Key Space if Table is CQL type
    if (tableInfo.tableType && tableInfo.tableType !== 'REDIS_TABLE_TYPE') {
      tableInfoItems.push({
        name: 'Key Space',
        data: tableInfo.tableDetails && tableInfo.tableDetails.keyspace
      });
    }

    return <DescriptionList listItems={tableInfoItems} />;
  }
}
