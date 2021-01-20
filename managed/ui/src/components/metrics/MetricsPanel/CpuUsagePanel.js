// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import { Graph } from '../';
import { isNonEmptyArray } from '../../../utils/ObjectUtils';
import { YBResourceCount } from '../../../components/common/descriptors';
import './MetricsPanel.scss';

export default class CpuUsagePanel extends Component {
  static propTypes = {};

  render() {
    const { isKubernetes, metric } = this.props;
    const usage = {
      system: undefined,
      user: undefined
    };

    try {
      if (isNonEmptyArray(metric.data)) {
        if (isKubernetes) {
          usage.system = parseFloat(
            metric.data.find((item) => item.name === 'cpu_usage').y.slice(-1)[0]
          );
        } else {
          usage.system = parseFloat(
            metric.data.find((item) => item.name === 'system').y.slice(-1)[0]
          );
          usage.user = parseFloat(metric.data.find((item) => item.name === 'user').y.slice(-1)[0]);
        }
      }
    } catch (err) {
      console.log('CPU metric processing failed with: ' + err);
    }
    const value = usage.system
      ? Math.round((usage.system + (usage.user !== undefined ? usage.user : 0)) * 10) / 1000
      : 0;
    return (
      <div className="metrics-padded-panel cpu-usage-panel">
        {isNaN(usage.system) ? (
          <Fragment>
            <Graph type={'semicircle'} value={0} />
            <div className="centered text-light text-lightgray">No Data</div>
          </Fragment>
        ) : (
          <Fragment>
            <Graph type={'semicircle'} value={value} />
            <YBResourceCount size={Math.round(value * 1000) / 10} kind="% used" inline={true} />
          </Fragment>
        )}
      </div>
    );
  }
}
