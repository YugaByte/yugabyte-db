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

import './Replication.scss';

const GRAPH_TYPE = 'replication';
const METRIC_NAME = 'tserver_async_replication_lag_micros';
const MICROS_IN_MIN = 60000000.00;
const MICROS_IN_SEC = 1000000.00;
const MICROS_IN_MS = 1000.00;

export default class ListBackups extends Component {
  static defaultProps = {
    title : "Replication"
  }

  static propTypes  = {
    currentUniverse: PropTypes.object.isRequired
  }

  componentDidMount() {
    const { graph } = this.props;
    this.queryMetrics(graph.graphFilter);    
  }

  componentWillUnmount() {
    this.props.resetMasterLeader();
  }

  queryMetrics = graphFilter => {
    const { currentUniverse } = this.props;
    const universeDetails = getPromiseState(currentUniverse).isSuccess() ?
      currentUniverse.data.universeDetails : 'all';
    const params = {
      metrics: [METRIC_NAME],
      start: graphFilter.startMoment.format('X'),
      end: graphFilter.endMoment.format('X'),
      nodePrefix: universeDetails.nodePrefix
    };
    this.props.queryMetrics(params, GRAPH_TYPE);
  };

  render() {
    const { title, currentUniverse, graph: { metrics }} = this.props;
    const universeDetails = currentUniverse.data.universeDetails;
    const nodeDetails = universeDetails.nodeDetailsSet;
    if (!isNonEmptyArray(nodeDetails)) {
      return <YBLoading />;
    }
    let latestStat = null;
    let latestTimestamp = null;
    let showMetrics = false;
    if (_.get(metrics, `${GRAPH_TYPE}.${METRIC_NAME}.layout.yaxis.alias`, null)) {
      // Get alias 
      const metricAliases = metrics[GRAPH_TYPE][METRIC_NAME].layout.yaxis.alias;
      const displayName = metricAliases['async_replication_committed_lag_micros']
      const replicationMetric = metrics[GRAPH_TYPE][METRIC_NAME].data.find(x => x.name === displayName);
      if (replicationMetric) {
        latestStat = replicationMetric.y[replicationMetric.y.length - 1];
        latestTimestamp = replicationMetric.x[replicationMetric.x.length - 1];
        showMetrics = true;
      }
    }
    
    let infoBlock = <span />
    let recentStatBlock = null;
    if (latestStat != null) {      
      if (latestStat == 0) {
        infoBlock = <div className="info success">
          <i className="fa fa-check-circle icon"></i>
          <div>
            <h4>Cluster is caught up!</h4>
          </div>
        </div>;
      }
      let resourceNumber = <YBResourceCount size={latestStat} kind="μs" inline={true} />;
      if (latestStat > MICROS_IN_MIN) {
        resourceNumber = <YBResourceCount size={(latestStat / MICROS_IN_MIN).toFixed(4)} kind="min" inline={true} />;
      } else if (latestStat > MICROS_IN_SEC) {
        resourceNumber = <YBResourceCount size={(latestStat / MICROS_IN_SEC).toFixed(4)} kind="s" inline={true} />;
      } else if (latestStat > MICROS_IN_MS) {
        resourceNumber = <YBResourceCount size={(latestStat / MICROS_IN_MS).toFixed(4)} kind="ms" inline={true} />;
      }
      recentStatBlock = <div className="metric-block">
        <h3>Current Replication Lag</h3>
        {resourceNumber}
        <div className="metric-attribute">as of {moment(latestTimestamp).fromNow()}</div>
      </div>;
    }

    return (
      <div>
        <YBPanelItem
          header={
            <div className="container-title clearfix spacing-top">
              <div className="pull-left">
                <h2 className="task-list-header content-title pull-left">{title}</h2>
              </div>              
            </div>
          }
          body={
           <div className="replication-content">
             {infoBlock}
              <div className="replication-content-stats">                
                {recentStatBlock}
              </div>
              {!showMetrics &&
                <div className="no-data">No data to display.</div>
              }
              {showMetrics && metrics[GRAPH_TYPE] && 
                <div className="graph-container">
                  <MetricsPanel 
                    metricKey={METRIC_NAME}
                    metric={metrics[GRAPH_TYPE][METRIC_NAME]}
                    className={"metrics-panel-container"}
                    width={1410}
                  />
                </div>
              }
            </div>
          }
        />
      </div>
    );
  }

}
