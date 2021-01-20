// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import moment from 'moment';
import _ from 'lodash';
import { isNonEmptyArray } from '../../../utils/ObjectUtils';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { YBPanelItem } from '../../panels';
import { YBLoading } from '../../common/indicators';
import { YBResourceCount } from '../../common/descriptors';
import { MetricsPanel } from '../../metrics';
import { ReplicationAlertModalBtn } from './ReplicationAlertModalBtn';
import './Replication.scss';

const GRAPH_TYPE = 'replication';
const METRIC_NAME = 'tserver_async_replication_lag_micros';
const MILLI_IN_MIN = 60000.0;
const MILLI_IN_SEC = 1000.0;

export default class Replication extends Component {
  constructor(props) {
    super(props);
    this.state = {
      graphWidth: 840
    };
  }

  static propTypes = {
    universe: PropTypes.object.isRequired
  };

  componentDidMount() {
    const { graph } = this.props;
    this.queryMetrics(graph.graphFilter);
  }

  componentWillUnmount() {
    this.props.resetMasterLeader();
  }

  queryMetrics = (graphFilter) => {
    const { universe: { currentUniverse }} = this.props;
    const universeDetails = getPromiseState(currentUniverse).isSuccess()
      ? currentUniverse.data.universeDetails
      : 'all';
    const params = {
      metrics: [METRIC_NAME],
      start: graphFilter.startMoment.format('X'),
      end: graphFilter.endMoment.format('X'),
      nodePrefix: universeDetails.nodePrefix
    };
    this.props.queryMetrics(params, GRAPH_TYPE);
  };

  render() {
    const {
      universe: { currentUniverse },
      graph: { metrics }
    } = this.props;

    const universeDetails = currentUniverse.data.universeDetails;
    const nodeDetails = universeDetails.nodeDetailsSet;
    if (!isNonEmptyArray(nodeDetails)) {
      return <YBLoading />;
    }
    let latestStat = null;
    let latestTimestamp = null;
    let showMetrics = false;
    let aggregatedMetrics = {};
    if (_.get(metrics, `${GRAPH_TYPE}.${METRIC_NAME}.layout.yaxis.alias`, null)) {
      // Get alias
      const metricAliases = metrics[GRAPH_TYPE][METRIC_NAME].layout.yaxis.alias;
      const committedLagName = metricAliases['async_replication_committed_lag_micros'];
      aggregatedMetrics = { ...metrics[GRAPH_TYPE][METRIC_NAME] };
      const replicationNodeMetrics = metrics[GRAPH_TYPE][METRIC_NAME].data.filter(
        (x) => x.name === committedLagName
      );
      if (replicationNodeMetrics.length) {
        // Get max-value and avg-value metric array
        let avgArr = null,
          maxArr = null;
        replicationNodeMetrics.forEach((metric) => {
          if (!avgArr && !maxArr) {
            avgArr = metric.y.map((v) => parseFloat(v) / replicationNodeMetrics.length);
            maxArr = [...metric.y];
          } else {
            metric.y.forEach((y, idx) => {
              avgArr[idx] = parseFloat(avgArr[idx]) + parseFloat(y) / replicationNodeMetrics.length;
              if (parseFloat(y) > parseFloat(maxArr[idx])) {
                maxArr[idx] = parseFloat(y);
              }
            });
          }
        });
        const firstMetricData = replicationNodeMetrics[0];
        aggregatedMetrics.data = [
          {
            ...firstMetricData,
            name: `Max ${committedLagName}`,
            y: maxArr
          },
          {
            ...firstMetricData,
            name: `Avg ${committedLagName}`,
            y: avgArr
          }
        ];
        latestStat = avgArr[avgArr.length - 1];
        latestTimestamp = firstMetricData.x[firstMetricData.x.length - 1];
        showMetrics = true;
      }
    }

    let infoBlock = <span />;
    let recentStatBlock = null;
    if (latestStat != null) {
      if (parseInt(latestStat) === 0) {
        infoBlock = (
          <div className="info success">
            <i className="fa fa-check-circle icon"></i>
            <div>
              <h4>Cluster is caught up!</h4>
            </div>
          </div>
        );
      }
      let resourceNumber = <YBResourceCount size={latestStat} kind="ms" inline={true} />;
      if (latestStat > MILLI_IN_MIN) {
        resourceNumber = (
          <YBResourceCount size={(latestStat / MILLI_IN_MIN).toFixed(4)} kind="min" inline={true} />
        );
      } else if (latestStat > MILLI_IN_SEC) {
        resourceNumber = (
          <YBResourceCount size={(latestStat / MILLI_IN_SEC).toFixed(4)} kind="s" inline={true} />
        );
      }
      recentStatBlock = (
        <div className="metric-block">
          <h3>Current Replication Lag</h3>
          {resourceNumber}
          <div className="metric-attribute">as of {moment(latestTimestamp).fromNow()}</div>
        </div>
      );
    }

    // TODO: Make graph resizeable
    return (
      <div>
        <YBPanelItem
          header={
            <div className="replication-header">
              <h2>Replication</h2>
              <ReplicationAlertModalBtn
                universeUUID={currentUniverse.data.universeUUID}
                disabled={!showMetrics}
              />
            </div>
          }
          body={
            <div className="replication-content">
              {infoBlock}
              <div className="replication-content-stats">{recentStatBlock}</div>
              {!showMetrics && <div className="no-data">No data to display.</div>}
              {showMetrics && metrics[GRAPH_TYPE] && (
                <div className="graph-container">
                  <MetricsPanel
                    metricKey={METRIC_NAME}
                    metric={aggregatedMetrics}
                    className={'metrics-panel-container'}
                    width={this.state.graphWidth}
                    height={540}
                  />
                </div>
              )}
            </div>
          }
        />
      </div>
    );
  }
};
