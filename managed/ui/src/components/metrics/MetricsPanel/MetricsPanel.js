// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import {
  removeNullProperties,
  isNonEmptyObject,
  isNonEmptyArray,
  isNonEmptyString,
  isYAxisGreaterThanThousand,
  divideYAxisByThousand
} from '../../../utils/ObjectUtils';
import './MetricsPanel.scss';
import { METRIC_FONT } from '../MetricsConfig';

const Plotly = require('plotly.js/lib/core');

const WIDTH_OFFSET = 23;
const CONTAINER_PADDING = 60;
const MAX_GRAPH_WIDTH_PX = 600;
const GRAPH_GUTTER_WIDTH_PX = 15;

export default class MetricsPanel extends Component {
  static propTypes = {
    metric: PropTypes.object.isRequired,
    metricKey: PropTypes.string.isRequired
  }

  plotGraph = () => {
    const { metricKey, metric } = this.props;
    if (isNonEmptyObject(metric)) {
      // Remove Null Properties from the layout
      removeNullProperties(metric.layout);
      // Detect if unit is µs and Y axis value is > 1000.
      // if so divide all Y axis values by 1000 and replace unit to ms.
      if (isNonEmptyObject(metric.layout.yaxis) && metric.layout.yaxis.ticksuffix === "&nbsp;µs" && isNonEmptyArray(metric.data)) {
        if (isYAxisGreaterThanThousand(metric.data)) {
          metric.data = divideYAxisByThousand(metric.data);
          metric.layout.yaxis.ticksuffix = "&nbsp;ms";
        }
      }

      metric.layout.xaxis.hoverformat = '%H:%M:%S, %b %d, %Y';

      // TODO: send this data from backend.
      let max = 0;
      metric.data.forEach(function (data) {
        if (data.y) {
          data.y.forEach(function (y) {
            y = parseFloat(y) * 1.25;
            if (y > max) max = y;
          });
        }
      });
      if (max === 0) max = 1.01;
      metric.layout.autosize = false;
      metric.layout.width = this.getGraphWidth(this.props.width || 1200);
      metric.layout.height = 360;
      metric.layout.showlegend = true;
      metric.layout.margin = {
        l: 45,
        r: 25,
        b: 0,
        t: 70,
        pad: 4,
      };
      if (isNonEmptyObject(metric.layout.yaxis) && isNonEmptyString(metric.layout.yaxis.ticksuffix)) {
        metric.layout.margin.l = 70;
        metric.layout.yaxis.range = [0, max];
      } else {
        metric.layout.yaxis = {range: [0, max]};
      }
      metric.layout.font = {
        family: METRIC_FONT,
      };
      metric.layout.legend = {
        orientation: "h",
        xanchor: "center",
        yanchor: "bottom",
        x: 0.5,
        y: -0.5,
      };

      // Handle the case when the metric data is empty, we would show
      // graph with No Data annotation.
      if (!isNonEmptyArray(metric.data)) {
        metric.layout["annotations"] = [{
          visible: true,
          align: "center",
          text: "No Data",
          showarrow: false,
          x: 1,
          y: 1
        }];
        metric.layout.margin.b = 105;
        metric.layout.xaxis = {range: [0, 2]};
        metric.layout.yaxis = {range: [0, 2]};
      }

      Plotly.newPlot(metricKey, metric.data, metric.layout, {displayModeBar: false});
    }
  }

  componentDidMount() {
    this.plotGraph();
  }

  componentDidUpdate(prevProps) {    
    if (this.props.width !== prevProps.width) {
      Plotly.relayout(prevProps.metricKey, {width: this.getGraphWidth(this.props.width)});
    } else {
      // All graph lines have the same x-axis, so get the first
      // and compare unix time interval
      const prevTime = prevProps.metric.data[0]?.x;
      const currTime = this.props.metric.data[0]?.x;      
      if (prevTime && currTime && (prevTime[0] !== currTime[0] ||
        prevTime[prevTime.length - 1] !== currTime[currTime.length - 1])) {
          // Re-plot graph
          this.plotGraph();
      }
    }    
  }

  getGraphWidth(containerWidth) {
    const width = containerWidth - CONTAINER_PADDING - WIDTH_OFFSET;
    const columnCount = Math.ceil(width / MAX_GRAPH_WIDTH_PX);
    return Math.floor(width / columnCount) - GRAPH_GUTTER_WIDTH_PX;
  }

  render() {
    return (
      <div id={this.props.metricKey} className="metrics-panel">
        <div />
      </div>
    );
  }
}
