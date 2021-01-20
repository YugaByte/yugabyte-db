// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { ListUniverseContainer } from '../components/universes';

export default class ListUniverse extends Component {
  render() {
    return (
      <div className="dashboard-container">
        <ListUniverseContainer />
      </div>
    );
  }
}
