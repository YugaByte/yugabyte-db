// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { isValidObject } from '../../../utils/ObjectUtils';

const Plotly = require('plotly.js/lib/core');

export default class UniverseReadWriteMetrics extends Component {
  componentDidUpdate(prevProps) {
    const { readData, writeData, graphIndex } = this.props;
    const data = this.preparePlotlyData([
      {
        data: readData,
        color: '#4477dd'
      },
      {
        data: writeData,
        color: '#cd6500'
      }
    ]);
    if (data.length) {
      let max = 0.01;
      data.forEach(function (series) {
        series.y.forEach(function (value) {
          if (parseFloat(value) > max) max = value;
        });
      });
      max = Math.round(max * 10) / 10;
      const layout = {
        margin: { l: 42, r: 0, t: 5, b: 8, pad: 0, autoexpand: false },
        xaxis: {
          showline: false,
          showgrid: false,
          zeroline: false,
          title: 'Last 1 Hour',
          titlefont: {
            size: 8,
            color: '#7f7f7f'
          }
        },
        yaxis: {
          tickvals: [0, max],
          range: [0, max * 1.1],
          gridwidth: 1,
          gridcolor: '#aaa7a3',
          showgrid: true,
          tickfont: {
            family: 'Inter, sans-serif',
            size: 9,
            color: '#777573'
          }
        },
        showlegend: false,
        autosize: false,
        height: 50,
        width: 140,
        paper_bgcolor: 'rgba(0,0,0,0)',
        plot_bgcolor: 'rgba(0,0,0,0)'
      };
      Plotly.newPlot(`lineGraph${graphIndex}`, data, layout, { displayModeBar: false });
    }
  }

  preparePlotlyData(lines) {
    const data = [];
    lines.forEach(function (line, index) {
      const plotName = index === 0 ? 'Read' : 'Write';
      if (isValidObject(line.data)) {
        data.push({
          x: line.data.x,
          y: line.data.y,
          mode: 'lines',
          line: {
            color: line.color,
            width: 2
          },
          name: plotName
        });
      }
    });
    return data;
  }

  render() {
    const { graphIndex } = this.props;
    return <div id={`lineGraph${graphIndex}`} />;
  }
}
