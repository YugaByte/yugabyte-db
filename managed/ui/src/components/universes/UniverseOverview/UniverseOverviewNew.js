// Copyright (c) YugaByte, Inc.

import React, { Component, PureComponent, Fragment } from 'react';
import { Link } from 'react-router';

import { Row, Col } from 'react-bootstrap';
import PropTypes from 'prop-types';
import { FormattedDate, FormattedRelative } from 'react-intl';
import { ClusterInfoPanelContainer, YBWidget } from '../../panels';
import {
  OverviewMetricsContainer,
  StandaloneMetricsPanelContainer,
  DiskUsagePanel,
  CpuUsagePanel
} from '../../metrics';
import {
  YBResourceCount,
  YBCost,
  DescriptionList,
  YBCodeBlock
} from '../../../components/common/descriptors';
import { RegionMap, YBMapLegend } from '../../maps';
import {
  isNonEmptyObject,
  isNullOrEmpty,
  isNonEmptyArray,
  isNonEmptyString
} from '../../../utils/ObjectUtils';
import { isKubernetesUniverse, getPrimaryCluster } from '../../../utils/UniverseUtils';
import { FlexContainer, FlexGrow, FlexShrink } from '../../common/flexbox/YBFlexBox';
import { isDefinedNotNull } from '../../../utils/ObjectUtils';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { YBButton, YBModal } from '../../common/forms/fields';
import moment from 'moment';
import pluralize from 'pluralize';
import { isEnabled, isDisabled } from '../../../utils/LayoutUtils';

class DatabasePanel extends PureComponent {
  static propTypes = {
    universeInfo: PropTypes.object.isRequired
  };

  render() {
    const {
      universeInfo: {
        universeDetails: { clusters }
      }
    } = this.props;
    const primaryCluster = getPrimaryCluster(clusters);
    const userIntent = primaryCluster && primaryCluster.userIntent;

    const optimizeVersion = (version) => {
      if (parseInt(version[version.length - 1], 10) === 0) {
        return optimizeVersion(version.slice(0, version.length - 1));
      } else {
        return version.join('.');
      }
    };
    return (
      <Row className={'overview-widget-database'}>
        <Col xs={12} className="centered">
          <YBResourceCount
            className="hidden-costs"
            size={optimizeVersion(userIntent.ybSoftwareVersion.split('-')[0].split('.'))}
            kind={'Version'}
          />
        </Col>
      </Row>
    );
  }
}

class HealthHeart extends PureComponent {
  static propTypes = {
    status: PropTypes.string
  };

  render() {
    const { status } = this.props;
    return (
      <div id="health-heart">
        <span className={`fa fa-heart${status === 'loading' ? ' status-loading' : ''}`}></span>
        {status === 'success' && (
          <div id="health-heartbeat">
            <svg
              x="0px"
              y="0px"
              viewBox="0 0 41.8 22.2"
              xmlns="http://www.w3.org/2000/svg"
              strokeLinejoin="round"
              strokeLinecap="round"
            >
              <polyline
                strokeLinejoin="round"
                strokeLinecap="round"
                points="38.3,11.9 29.5,11.9 27.6,9 24,18.6 21.6,3.1 18.6,11.9 2.8,11.9 "
              />
            </svg>
          </div>
        )}

        {status === 'error' && (
          <div id="health-droplet">
            <svg
              x="0px"
              y="0px"
              width="264.564px"
              height="264.564px"
              viewBox="0 0 264.564 264.564"
              xmlns="http://www.w3.org/2000/svg"
              strokeLinejoin="round"
              strokeLinecap="round"
            >
              <path
                strokeLinejoin="round"
                strokeLinecap="round"
                d="M132.281,264.564c51.24,0,92.931-41.681,92.931-92.918c0-50.18-87.094-164.069-90.803-168.891L132.281,0l-2.128,2.773 c-3.704,4.813-90.802,118.71-90.802,168.882C39.352,222.883,81.042,264.564,132.281,264.564z"
              />
            </svg>
          </div>
        )}
      </div>
    );
  }
}

class AlertInfoPanel extends PureComponent {
  static propTypes = {
    alerts: PropTypes.object,
    universeInfo: PropTypes.object
  };

  render() {
    const { alerts, universeInfo } = this.props;
    const errorNodesCounter = alerts.alertsList.length;

    const errorText = errorNodesCounter + ' ' + pluralize('Alert', errorNodesCounter);
    let errorSpan = <span className="text-red text-light">{errorText}</span>;
    let errorHeader = <span className="fa fa-exclamation-triangle text-red" />;
    if (errorNodesCounter && isNonEmptyObject(universeInfo)) {
      errorSpan = (
        <Link className="text-red text-regular" to={'/alerts'}>
          {errorText}
        </Link>
      );
      errorHeader = <Link className="fa fa-exclamation-triangle text-red" to={'/alerts'} />;
    }
    if (alerts.alertsList) {
      const lastUpdateDate = alerts.updated && moment(alerts.updated);
      const healthCheckInfoItems = [
        {
          name: '',
          data: errorNodesCounter ? (
            errorSpan
          ) : (
            <span className="text-green text-light">
              <i className={'fa fa-check'}></i> All running fine
            </span>
          )
        },
        {
          name: '',
          data: lastUpdateDate ? (
            <span className="text-lightgray text-light">
              <i className={'fa fa-clock-o'}></i> Updated{' '}
              <span className={'text-dark text-normal'}>
                <FormattedRelative value={lastUpdateDate} />
              </span>
            </span>
          ) : null
        }
      ];

      return (
        <YBWidget
          size={1}
          className={'overview-widget-cluster-primary'}
          headerLeft={'Alerts'}
          headerRight={errorNodesCounter ? errorHeader : <Link to={`/alerts`}>Details</Link>}
          body={
            <FlexContainer className={'centered health-heart-cnt'} direction={'row'}>
              <FlexGrow>
                <HealthHeart status={errorNodesCounter ? 'error' : 'success'} />
              </FlexGrow>
              <FlexGrow>
                <DescriptionList
                  type={'inline'}
                  className={'health-check-legend'}
                  listItems={healthCheckInfoItems}
                />
              </FlexGrow>
            </FlexContainer>
          }
        />
      );
    }

    const errorContent = {
      heartStatus: 'empty',
      body: 'No finished checks'
    };

    return (
      <YBWidget
        size={1}
        className={'overview-widget-cluster-primary'}
        headerLeft={'Alerts'}
        headerRight={errorNodesCounter ? errorHeader : <Link to={`/alerts`}>Details</Link>}
        body={
          <FlexContainer className={'centered health-heart-cnt'} direction={'column'}>
            <FlexGrow>
              <HealthHeart status={errorNodesCounter ? 'error' : 'success'} />
            </FlexGrow>
            {isNonEmptyString(errorContent.body) && (
              <FlexShrink
                className={errorContent.heartStatus === 'empty' ? 'text-light text-lightgray' : ''}
              >
                {errorContent.body}
              </FlexShrink>
            )}
          </FlexContainer>
        }
      />
    );
  }
}

class HealthInfoPanel extends PureComponent {
  static propTypes = {
    healthCheck: PropTypes.object.isRequired,
    universeInfo: PropTypes.object.isRequired
  };

  render() {
    const { healthCheck, universeInfo } = this.props;
    let disabledUntilStr = '';
    if (getPromiseState(healthCheck).isSuccess()) {
      const healthCheckData = JSON.parse([...healthCheck.data].reverse()[0]);
      const lastUpdateDate = moment(healthCheckData.timestamp);
      if (universeInfo.universeConfig && 'disableAlertsUntilSecs' in universeInfo.universeConfig) {
        const disabledUntilSecs = Number(universeInfo.universeConfig.disableAlertsUntilSecs);
        const now = Date.now() / 1000;
        if (!Number.isSafeInteger(disabledUntilSecs)) {
          disabledUntilStr = ' Alerts are snoozed';
        } else if (disabledUntilSecs > now) {
          disabledUntilStr =
            ' Alerts are snoozed until ' + moment.unix(disabledUntilSecs).format('MMM DD hh:mm a');
        }
      }
      const totalNodesCounter = healthCheckData.data.length;
      let errorNodesCounter = 0;

      healthCheckData.data.forEach((check) => {
        if (check.has_error) errorNodesCounter++;
      });

      const errorText = errorNodesCounter + ' ' + pluralize('Error', errorNodesCounter);
      let errorSpan = <span className="text-red text-light">{errorText}</span>;
      let errorHeader = <span className="fa fa-exclamation-triangle text-red" />;
      if (errorNodesCounter && isNonEmptyObject(universeInfo)) {
        errorSpan = (
          <Link
            className="text-red text-regular"
            to={`/universes/${universeInfo.universeUUID}/health`}
          >
            {errorText}
          </Link>
        );
        errorHeader = (
          <Link
            className="fa fa-exclamation-triangle text-red"
            to={`/universes/${universeInfo.universeUUID}/health`}
          />
        );
      }

      const healthCheckInfoItems = [
        {
          name: '',
          data: errorNodesCounter ? (
            errorSpan
          ) : totalNodesCounter ? (
            <span className="text-green text-light">
              <i className={'fa fa-check'}></i> All running fine
            </span>
          ) : (
            <span className="text-light">No finished check</span>
          )
        },
        {
          name: '',
          data: lastUpdateDate ? (
            <span className="text-lightgray text-light">
              <i className={'fa fa-clock-o'}></i> Updated{' '}
              <span className={'text-dark text-normal'}>
                <FormattedRelative value={lastUpdateDate} />
              </span>
            </span>
          ) : null
        },
        {
          name: '',
          data: disabledUntilStr ? (
            <span className="text-light">
              <i className={'fa fa-exclamation-triangle'}>{disabledUntilStr}</i>
            </span>
          ) : null
        }
      ];

      return (
        <YBWidget
          size={1}
          className={'overview-widget-cluster-primary'}
          headerLeft={'Health Check'}
          headerRight={
            errorNodesCounter ? (
              errorHeader
            ) : (
              <Link to={`/universes/${universeInfo.universeUUID}/health`}>Details</Link>
            )
          }
          body={
            <FlexContainer className={'centered health-heart-cnt'} direction={'row'}>
              <FlexGrow>
                <HealthHeart status={errorNodesCounter ? 'error' : 'success'} />
              </FlexGrow>
              <FlexGrow>
                <DescriptionList
                  type={'inline'}
                  className={'health-check-legend'}
                  listItems={healthCheckInfoItems}
                />
              </FlexGrow>
            </FlexContainer>
          }
        />
      );
    }

    const errorContent = {};
    if (getPromiseState(healthCheck).isEmpty()) {
      errorContent.heartStatus = 'empty';
      errorContent.body = 'No finished checks';
    }
    if (getPromiseState(healthCheck).isError()) {
      errorContent.heartStatus = 'empty';
      errorContent.body = 'Cannot get checks';
    }
    if (getPromiseState(healthCheck).isLoading()) {
      errorContent.heartStatus = 'loading';
      errorContent.body = '';
    }
    return (
      <YBWidget
        size={1}
        className={'overview-widget-cluster-primary'}
        headerLeft={'Health Check'}
        body={
          <FlexContainer className={'centered health-heart-cnt'} direction={'column'}>
            <FlexGrow>
              <HealthHeart status={errorContent.heartClassName} />
            </FlexGrow>
            {isNonEmptyString(errorContent.body) && (
              <FlexShrink
                className={errorContent.heartStatus === 'empty' ? 'text-light text-lightgray' : ''}
              >
                {errorContent.body}
              </FlexShrink>
            )}
          </FlexContainer>
        }
      />
    );
  }
}

export default class UniverseOverviewNew extends Component {
  hasReadReplica = (universeInfo) => {
    const clusters = universeInfo.universeDetails.clusters;
    return clusters.some((cluster) => cluster.clusterType === 'ASYNC');
  };

  getLastUpdateDate = () => {
    const universeTasks = this.tasksForUniverse();
    if (isNonEmptyArray(universeTasks)) {
      const updateTask = universeTasks.find((taskItem) => {
        return taskItem.type === 'UpgradeSoftware';
      });
      return isDefinedNotNull(updateTask)
        ? updateTask.completionTime || updateTask.createTime
        : null;
    }
    return null;
  };

  tasksForUniverse = () => {
    const {
      universe: {
        currentUniverse: {
          data: { universeUUID }
        }
      },
      tasks: { customerTaskList }
    } = this.props;
    const resultTasks = [];
    if (isNonEmptyArray(customerTaskList)) {
      customerTaskList.forEach((taskItem) => {
        if (taskItem.targetUUID === universeUUID) resultTasks.push(taskItem);
      });
    }
    return resultTasks;
  };

  getCostWidget = (currentUniverse) => {
    if (isNullOrEmpty(currentUniverse.resources)) return;
    const costPerDay = <YBCost value={currentUniverse.resources.pricePerHour} multiplier={'day'} />;
    const costPerMonth = (
      <YBCost value={currentUniverse.resources.pricePerHour} multiplier={'month'} />
    );
    return (
      <Col lg={2} md={4} sm={4} xs={6}>
        <YBWidget
          size={1}
          className={'overview-widget-cost'}
          headerLeft={'Cost'}
          body={
            <FlexContainer className={'centered'} direction={'column'}>
              <FlexGrow>
                <YBResourceCount
                  className="hidden-costs"
                  size={costPerDay}
                  kind="/day"
                  inline={true}
                />
              </FlexGrow>
              <FlexShrink>{costPerMonth} /month</FlexShrink>
            </FlexContainer>
          }
        />
      </Col>
    );
  };

  getDemoWidget = () => {
    const {
      closeModal,
      showDemoCommandModal,
      modal: { showModal, visibleModal }
    } = this.props;
    return (
      <Col lg={2} md={4} sm={4} xs={6}>
        <YBWidget
          size={1}
          className={'overview-widget-cost'}
          headerLeft={'Explore YSQL'}
          body={
            <FlexContainer direction={'column'}>
              <FlexGrow>
                <div style={{ marginBottom: '30px' }}>
                  Load a data set and run queries against it.
                </div>
              </FlexGrow>
              <FlexShrink className={'centered'}>
                <Fragment>
                  <YBButton
                    btnClass={'btn btn-default'}
                    btnText={'Create Demo'}
                    title={'Create Demo'}
                    onClick={showDemoCommandModal}
                  />
                  <YBModal
                    title={'YSQL Retail Demo'}
                    visible={showModal && visibleModal === 'universeOverviewDemoModal'}
                    onHide={closeModal}
                    cancelLabel={'Close'}
                    showCancelButton={true}
                  >
                    <div>Query a sample database:</div>
                    <YBCodeBlock>yugabyted demo connect</YBCodeBlock>
                    <div>
                      Explore YSQL at{' '}
                      <a href="https://docs.yugabyte.com/latest/quick-start/explore-ysql/">here</a>.
                    </div>
                  </YBModal>
                </Fragment>
              </FlexShrink>
            </FlexContainer>
          }
        />
      </Col>
    );
  };

  getPrimaryClusterWidget = (currentUniverse) => {
    if (isNullOrEmpty(currentUniverse)) return;
    return (
      <Col lg={2} sm={4} xs={6}>
        <ClusterInfoPanelContainer type={'primary'} universeInfo={currentUniverse} />
      </Col>
    );
  };

  getTablesWidget = (universeInfo) => {
    if (isNullOrEmpty(this.props.tables)) return;
    const { tables } = this.props;

    let numCassandraTables = 0;
    let numRedisTables = 0;
    let numPostgresTables = 0;
    if (isNonEmptyArray(tables.universeTablesList)) {
      tables.universeTablesList.forEach((table, idx) => {
        if (table.tableType === 'REDIS_TABLE_TYPE') {
          numRedisTables++;
        } else if (table.tableType === 'YQL_TABLE_TYPE') {
          numCassandraTables++;
        } else {
          numPostgresTables++;
        }
      });
    }

    return (
      <YBWidget
        size={1}
        className={'overview-widget-tables'}
        headerLeft={'Tables'}
        headerRight={
          isNonEmptyObject(universeInfo) ? (
            <Link to={`/universes/${universeInfo.universeUUID}/tables`}>Details</Link>
          ) : null
        }
        body={
          <FlexContainer className={'centered'}>
            <FlexGrow>
              <YBResourceCount size={numPostgresTables} kind="YSQL" />
            </FlexGrow>
            <FlexGrow>
              <YBResourceCount size={numCassandraTables} kind="YCQL" />
            </FlexGrow>
            <FlexGrow>
              <YBResourceCount size={numRedisTables} kind="YEDIS" />
            </FlexGrow>
          </FlexContainer>
        }
      />
    );
  };

  getHealthWidget = (healthCheck, universeInfo) => {
    return (
      <Col lg={4} md={8} sm={8} xs={12}>
        <HealthInfoPanel healthCheck={healthCheck} universeInfo={universeInfo} />
      </Col>
    );
  };

  getAlertWidget = (alerts, universeInfo) => {
    return (
      <Col lg={4} md={8} sm={8} xs={12}>
        <AlertInfoPanel alerts={alerts} universeInfo={universeInfo} />
      </Col>
    );
  };

  getDiskUsageWidget = (universeInfo) => {
    // For kubernetes the disk usage would be in container tab, rest it would be server tab.
    const isKubernetes = isKubernetesUniverse(universeInfo);
    const subTab = isKubernetes ? 'container' : 'server';
    const metricKey = isKubernetes ? 'container_volume_stats' : 'disk_usage';
    const secondaryMetric = isKubernetes
      ? [
        {
          metric: 'container_volume_max_usage',
          name: 'size'
        }
      ]
      : null;
    return (
      <StandaloneMetricsPanelContainer
        metricKey={metricKey}
        additionalMetricKeys={secondaryMetric}
        type="overview"
      >
        {(props) => {
          return (
            <YBWidget
              noMargin
              headerRight={
                isNonEmptyObject(universeInfo) ? (
                  <Link to={`/universes/${universeInfo.universeUUID}/metrics?subtab=${subTab}`}>
                    Details
                  </Link>
                ) : null
              }
              headerLeft={props.metric.layout.title}
              body={<DiskUsagePanel metric={props.metric} className={'disk-usage-container'} />}
            />
          );
        }}
      </StandaloneMetricsPanelContainer>
    );
  };

  getCPUWidget = (universeInfo) => {
    const isItKubernetesUniverse = isKubernetesUniverse(universeInfo);
    return (
      <Col lg={2} md={4} sm={4} xs={6}>
        <StandaloneMetricsPanelContainer
          metricKey={isItKubernetesUniverse ? 'container_cpu_usage' : 'cpu_usage'}
          type="overview"
        >
          {(props) => {
            return (
              <YBWidget
                noMargin
                headerLeft={'CPU Usage'}
                headerRight={
                  <Link to={`/universes/${universeInfo.universeUUID}/metrics?subtab=server`}>
                    Details
                  </Link>
                }
                body={
                  <CpuUsagePanel
                    metric={props.metric}
                    className={'disk-usage-container'}
                    isKubernetes={isItKubernetesUniverse}
                  />
                }
              />
            );
          }}
        </StandaloneMetricsPanelContainer>
      </Col>
    );
  };

  getRegionMapWidget = (universeInfo) => {
    const isItKubernetesUniverse = isKubernetesUniverse(universeInfo);
    const {
      modal: { showModal, visibleModal },
      showUniverseOverviewMapModal,
      closeModal
    } = this.props;
    const mapWidget = (
      <YBWidget
        numNode
        noMargin
        size={2}
        headerRight={
          <Fragment>
            <YBButton
              btnClass={'btn-clear'}
              btnIcon={'fa fa-expand'}
              onClick={showUniverseOverviewMapModal}
            />
            <YBModal
              onHide={closeModal}
              className={'modal-map-overview'}
              title={'Map'}
              visible={showModal && visibleModal === 'universeOverviewMapModal'}
            >
              <YBButton
                btnIcon={'fa fa-times'}
                btnClass={'btn btn-default btn-round button-close-map'}
                onClick={closeModal}
              />
              <RegionMap universe={universeInfo} type={'Universe'} setBounds={false} />
              <YBMapLegend
                title="Data Placement (In AZs)"
                clusters={universeInfo.universeDetails.clusters}
                type="Universe"
              />
            </YBModal>
          </Fragment>
        }
        headerLeft={isItKubernetesUniverse ? 'Universe Pods' : 'Universe Nodes'}
        body={
          <div>
            <RegionMap universe={universeInfo} type={'Universe'} setBounds={false} />
            <YBMapLegend
              title="Data Placement (In AZs)"
              clusters={universeInfo.universeDetails.clusters}
              type="Universe"
            />
          </div>
        }
      />
    );

    return (
      <Col lg={4} xs={12}>
        {mapWidget}
      </Col>
    );
  };

  getDatabaseWidget = (universeInfo, tasks) => {
    const lastUpdateDate = this.getLastUpdateDate();
    const { updateAvailable, currentCustomer } = this.props;
    const showUpdate =
      updateAvailable && !isDisabled(currentCustomer.data.features, 'universes.actions');

    const infoWidget = (
      <YBWidget
        headerLeft={'Info'}
        headerRight={
          showUpdate ? (
            <a onClick={(e) => {
              this.props.showSoftwareUpgradesModal(e);
              e.preventDefault();
            }} href="/">
              Upgrade <span className="badge badge-pill badge-orange">{updateAvailable}</span>
            </a>
          ) : null
        }
        body={
          <FlexContainer className={'centered'} direction={'column'}>
            <FlexGrow>
              <DatabasePanel universeInfo={universeInfo} tasks={tasks} />
            </FlexGrow>
            <FlexShrink>
              {lastUpdateDate && (
                <div className="text-lightgray text-light">
                  <span className={'fa fa-clock-o'}></span> Upgraded{' '}
                  <span className={'text-dark text-normal'}>
                    <FormattedDate
                      value={lastUpdateDate}
                      year="numeric"
                      month="short"
                      day="2-digit"
                    />
                  </span>
                </div>
              )}
            </FlexShrink>
          </FlexContainer>
        }
      />
    );
    return (
      <Col lg={2} md={4} sm={4} xs={6}>
        {infoWidget}
      </Col>
    );
  };

  render() {
    const {
      universe,
      universe: { currentUniverse },
      alerts,
      tasks,
      currentCustomer,
    } = this.props;

    const universeInfo = currentUniverse.data;
    const nodePrefixes = [universeInfo.universeDetails.nodePrefix];
    const isItKubernetesUniverse = isKubernetesUniverse(universeInfo);
    return (
      <Fragment>
        <Row>
          {this.getDatabaseWidget(universeInfo, tasks)}
          {this.getPrimaryClusterWidget(universeInfo)}
          {isEnabled(currentCustomer.data.features, 'universes.details.overview.costs') &&
            this.getCostWidget(universeInfo)}
          {isEnabled(
            currentCustomer.data.features,
            'universes.details.overview.demo',
            'disabled'
          ) && this.getDemoWidget()}
          {this.getCPUWidget(universeInfo)}
          {isDisabled(currentCustomer.data.features, 'universes.details.health')
            ? this.getAlertWidget(alerts, universeInfo)
            : this.getHealthWidget(universe.healthCheck, universeInfo)}
        </Row>
        <Row>
          {this.getRegionMapWidget(universeInfo)}

          <Col lg={4} xs={12} md={6} sm={6}>
            <OverviewMetricsContainer
              universeUuid={universeInfo.universeUUID}
              type={'overview'}
              origin={'universe'}
              nodePrefixes={nodePrefixes}
              isKubernetesUniverse={isItKubernetesUniverse}
            />
          </Col>
          <Col lg={4} md={6} sm={6} xs={12}>
            {this.getDiskUsageWidget(universeInfo)}
            {this.getTablesWidget(universeInfo)}
          </Col>
        </Row>
      </Fragment>
    );
  }
}
