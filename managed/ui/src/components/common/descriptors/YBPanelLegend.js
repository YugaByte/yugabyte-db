// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';

import './stylesheets/YBPanelLegend.scss';

export default class YBPanelLegend extends Component {
  render() {
    const { data } = this.props;
    return (
      <div className="panel-legend">
        {data.map((item, index) => (
          <div key={index} className="panel-legend-item">
            <span style={{ backgroundColor: item.color }}></span>
            {item.title}
          </div>
        ))}
      </div>
    );
  }
}
