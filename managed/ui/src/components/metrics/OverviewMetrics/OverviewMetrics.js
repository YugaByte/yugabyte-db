// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { MetricsPanelOverview } from '../';
import './OverviewMetrics.scss';
import { YBLoading } from '../../common/indicators';
import {
  isNonEmptyObject,
  isNonEmptyArray,
  isEmptyArray,
  isNonEmptyString
} from '../../../utils/ObjectUtils';
import { YBPanelLegend } from '../../common/descriptors';
import { YBWidget } from '../../panels';
import { METRIC_COLORS } from '../MetricsConfig';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { getFeatureState } from '../../../utils/LayoutUtils';
import _ from 'lodash';
import moment from 'moment';

// TODO set predefined defaults another way not to share defaults this way
const OVERVIEW_METRICS_INTERVAL_MS = 15000;

const panelTypes = {
  overview: {
    title: 'Overview',
    metrics: ['total_rpcs_per_sec', 'tserver_ops_latency', 'cpu_usage', 'disk_usage']
  }
};
const kubernetesMetrics = [
  'container_cpu_usage',
  'container_memory_usage',
  'container_volume_stats',
  'container_volume_max_usage'
];

class OverviewMetrics extends Component {
  static propTypes = {
    type: PropTypes.oneOf(Object.keys(panelTypes)).isRequired,
    nodePrefixes: PropTypes.array
  };
  static defaultProps = {
    nodePrefixes: []
  };
  constructor(props) {
    super(props);
    const refreshMetrics =
      localStorage.getItem('__yb_refresh_metrics__') != null &&
      localStorage.getItem('__yb_refresh_metrics__') !== 'false';
    this.state = {
      autoRefresh: refreshMetrics
    };
  }

  componentDidMount() {
    const { currentCustomer } = this.props;
    const { autoRefresh } = this.state;
    const self = this;
    const pollingInterval = getPromiseState(currentCustomer).isSuccess()
      ? getFeatureState(
        currentCustomer.data.features,
        'universes.details.overview.metricsInterval',
        OVERVIEW_METRICS_INTERVAL_MS
      )
      : OVERVIEW_METRICS_INTERVAL_MS;

    // set the polling for metrics but update start and end time interval boundaries
    self.queryMetricsType({
      ...self.props.graph.graphFilter,
      startMoment: moment().subtract('1', 'hours'),
      endMoment: moment()
    });
    if (autoRefresh) {
      this.timeout = setInterval(() => {
        self.queryMetricsType({
          ...self.props.graph.graphFilter,
          startMoment: moment().subtract('1', 'hours'),
          endMoment: moment()
        });
      }, pollingInterval);
    }
  }

  handleToggleRefresh = () => {
    const { currentCustomer, graph } = this.props;
    const { autoRefresh } = this.state;
    const pollingInterval = getPromiseState(currentCustomer).isSuccess()
      ? getFeatureState(
        currentCustomer.data.features,
        'universes.details.overview.metricsInterval',
        OVERVIEW_METRICS_INTERVAL_MS
      )
      : OVERVIEW_METRICS_INTERVAL_MS;

    // eslint-disable-next-line eqeqeq
    if (!autoRefresh && this.timeout == undefined) {
      this.timeout = setInterval(() => {
        this.queryMetricsType({
          ...graph.graphFilter,
          startMoment: moment().subtract('1', 'hours'),
          endMoment: moment()
        });
      }, pollingInterval);
    } else {
      clearInterval(this.timeout);
      delete this.timeout;
    }
    localStorage.setItem('__yb_refresh_metrics__', !autoRefresh);
    this.setState({ autoRefresh: !autoRefresh });
  };

  queryMetricsType = (graphFilter) => {
    const { startMoment, endMoment, nodeName, nodePrefix } = graphFilter;
    const { type, isKubernetesUniverse } = this.props;
    const params = {
      metrics: panelTypes[type].metrics,
      start: startMoment.format('X'),
      end: endMoment.format('X')
    };
    if (isNonEmptyString(nodePrefix) && nodePrefix !== 'all') {
      params.nodePrefix = nodePrefix;
    }
    if (isNonEmptyString(nodeName) && nodeName !== 'all') {
      params.nodeName = nodeName;
    }
    // In case of universe metrics , nodePrefix comes from component itself
    if (isNonEmptyArray(this.props.nodePrefixes)) {
      params.nodePrefix = this.props.nodePrefixes[0];
    }
    if (isNonEmptyString(this.props.tableName)) {
      params.tableName = this.props.tableName;
    }
    this.props.queryMetrics(params, type);
    if (isKubernetesUniverse) {
      this.props.queryMetrics(
        {
          ...params,
          metrics: kubernetesMetrics
        },
        type
      );
    }
  };

  componentWillUnmount() {
    this.props.resetMetrics();
    // eslint-disable-next-line eqeqeq
    if (this.timeout != undefined) {
      clearInterval(this.timeout);
      delete this.timeout;
    }
  }

  render() {
    const {
      type,
      graph: { metrics }
    } = this.props;
    const { autoRefresh } = this.state;
    const metricKeys = panelTypes[type].metrics;
    let panelItem = metricKeys
      .filter((metricKey) => metricKey !== 'disk_usage' && metricKey !== 'cpu_usage')
      .map(function (metricKey, idx) {
        return <YBWidget key={type + idx} noMargin body={<YBLoading />} />;
      });
    if (Object.keys(metrics).length > 0 && isNonEmptyObject(metrics[type])) {
      /* Logic here is, since there will be multiple instances of GraphPanel
      we basically would have metrics data keyed off panel type. So we
      loop through all the possible panel types in the metric data fetched
      and group metrics by panel type and filter out anything that is empty.
      */
      panelItem = metricKeys
        .map((metricKey, idx) => {
          // skip disk_usage and cpu_usage due to separate widget
          if (metricKey !== 'disk_usage' && metricKey !== 'cpu_usage') {
            if (isNonEmptyObject(metrics[type][metricKey]) && !metrics[type][metricKey].error) {
              const legendData = [];
              for (let idx = 0; idx < metrics[type][metricKey].data.length; idx++) {
                metrics[type][metricKey].data[idx].fill = 'tozeroy';
                metrics[type][metricKey].data[idx].fillcolor = METRIC_COLORS[idx] + '10';
                metrics[type][metricKey].data[idx].line = {
                  color: METRIC_COLORS[idx],
                  width: 1.5
                };
                legendData.push({
                  color: METRIC_COLORS[idx],
                  title: metrics[type][metricKey].data[idx].name
                });
              }
              const metricTickSuffix = _.get(metrics[type][metricKey], 'layout.yaxis.ticksuffix');
              const measureUnit = metricTickSuffix
                ? ` (${metricTickSuffix.replace('&nbsp;', '')})`
                : '';
              return (
                <YBWidget
                  key={idx}
                  noMargin
                  headerRight={
                    metricKey === 'disk_usage' ? null : <YBPanelLegend data={legendData} />
                  }
                  headerLeft={
                    <div className="metric-title">
                      <span>{metrics[type][metricKey].layout.title + measureUnit}</span>
                      <i
                        className={autoRefresh ? 'fa fa-pause' : 'fa fa-refresh'}
                        title={
                          autoRefresh
                            ? 'Click to pause auto-refresh'
                            : 'Click to enable auto-refresh'
                        }
                        onClick={this.handleToggleRefresh}
                      ></i>
                    </div>
                  }
                  body={
                    <MetricsPanelOverview
                      metricKey={metricKey}
                      metric={metrics[type][metricKey]}
                      className={'metrics-panel-container'}
                    />
                  }
                />
              );
            }
            return (
              <YBWidget
                key={type + idx}
                noMargin
                headerLeft={
                  metricKey.replace(/_/g, ' ').charAt(0).toUpperCase() +
                  metricKey.replace(/_/g, ' ').slice(1)
                }
                body={
                  <MetricsPanelOverview
                    metricKey={metricKey}
                    metric={{
                      data: [],
                      layout: {
                        xaxis: {},
                        yaxis: {}
                      }
                    }}
                    className={'metrics-panel-container'}
                  />
                }
              />
            );
          }
          return null;
        })
        .filter(Boolean);
    }
    let panelData = panelItem;
    if (isEmptyArray(panelItem)) {
      panelData = 'Error receiving response from Graph Server';
    }
    return panelData;
  }
}

export default OverviewMetrics;
