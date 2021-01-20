// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import Universes from './Universes';
import { TableDetailContainer } from '../components/tables';

export default class TableDetail extends Component {
  render() {
    return (
      <Universes>
        <TableDetailContainer
          universeUUID={this.props.params.uuid}
          tableUUID={this.props.params.tableUUID}
        />
      </Universes>
    );
  }
}
