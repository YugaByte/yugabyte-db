// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';

import { CustomerMetricsPanelContainer } from '../components/metrics';
import Measure from 'react-measure';

class Metrics extends Component {
  state = {
    dimensions: {}
  };

  onResize(dimensions) {
    this.setState({ dimensions });
  }
  render() {
    return (
      <Measure onMeasure={this.onResize.bind(this)}>
        <div className="dashboard-container">
          <CustomerMetricsPanelContainer origin={'customer'} width={this.state.dimensions.width} />
        </div>
      </Measure>
    );
  }
}

export default Metrics;
