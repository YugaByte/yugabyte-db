// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import AlertsListContainer from '../components/alerts/AlertList/AlertsListContainer';

export default class Alerts extends Component {
  render() {
    return (
      <div className="dashboard-container">
        <AlertsListContainer />
      </div>
    );
  }
}
