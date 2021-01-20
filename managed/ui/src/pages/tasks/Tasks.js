// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';

export default class Tasks extends Component {
  render() {
    return <div className="dashboard-container">{this.props.children}</div>;
  }
}
