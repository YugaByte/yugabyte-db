// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import { Graph } from '../';
import { isNonEmptyArray } from '../../../utils/ObjectUtils';
import { YBResourceCount } from '../../../components/common/descriptors';
import './MetricsPanel.scss';

export default class DiskUsagePanel extends Component {
  static propTypes = {};

  render() {
    const { metric } = this.props;
    const space = {
      free: undefined,
      used: undefined,
      size: undefined
    };
    if (isNonEmptyArray(metric.data)) {
      const diskUsedObj = metric.data.find((item) => item.name === 'used');
      const diskFreeObj = metric.data.find((item) => item.name === 'free');
      const diskSizeObj = metric.data.find((item) => item.name === 'size');
      const getLastElement = (arr) => arr && arr.length && arr[arr.length - 1];

      // If at least two out of three objects are defined we can figure out the rest
      if (!!diskUsedObj && !!diskFreeObj) {
        space.used = getLastElement(diskUsedObj.y);
        space.free = getLastElement(diskFreeObj.y);
        space.size = space.used + space.free;
      } else if (!!diskUsedObj && !!diskSizeObj) {
        space.used = getLastElement(diskUsedObj.y);
        space.size = getLastElement(diskSizeObj.y);
        space.free = space.size - space.used;
      } else if (!!diskFreeObj && !!diskSizeObj) {
        space.free = getLastElement(diskFreeObj.y);
        space.size = getLastElement(diskSizeObj.y);
        space.used = space.size - space.free;
      } else {
        console.error(
          `Metric missing properties. Free: ${diskFreeObj}, Used: ${diskUsedObj}, Size: ${diskSizeObj}`
        );
      }
    }
    const value = space.size ? Math.round((space.used * 1000) / space.size) / 1000 : 0;
    return (
      <div className="metrics-padded-panel disk-usage-panel">
        {isNaN(space.size) ? (
          <Fragment>
            <YBResourceCount size={'No Data'} />
            <span className="gray-text metric-subtitle">{'Data is unavailable'} </span>
            <Graph value={0} unit={'percent'} />
          </Fragment>
        ) : (
          <Fragment>
            <YBResourceCount size={Math.round(space.used * 10) / 10} unit="GB used" />
            {space.free && (
              <span className="gray-text metric-subtitle">
                {Math.round(space.free)} GB free out of {Math.round(space.size)} GB
              </span>
            )}
            <Graph value={value} unit={'percent'} />
          </Fragment>
        )}
      </div>
    );
  }
}
