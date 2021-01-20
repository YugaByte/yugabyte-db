import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { Field } from 'redux-form';
import {
  YBControlledSelect,
  YBControlledNumericInput,
  YBCheckBox,
  YBButton
} from '../../common/forms/fields';
import { Row, Col } from 'react-bootstrap';
import _ from 'lodash';
import {
  isNonEmptyArray,
  isDefinedNotNull,
  areUniverseConfigsEqual,
  isEmptyObject,
  isNonEmptyObject
} from '../../../utils/ObjectUtils';
import { FlexContainer, FlexShrink, FlexGrow } from '../../common/flexbox/YBFlexBox';
import {
  getPrimaryCluster,
  getReadOnlyCluster,
  getClusterByType
} from '../../../utils/UniverseUtils';
import { getPromiseState } from '../../../utils/PromiseUtils';

const nodeStates = {
  activeStates: [
    'ToBeAdded',
    'Provisioned',
    'SoftwareInstalled',
    'UpgradeSoftware',
    'UpdateGFlags',
    'Live',
    'Starting'
  ],
  inactiveStates: [
    'Unreachable',
    'ToBeRemoved',
    'Removing',
    'Removed',
    'Decommissioned',
    'BeingDecommissioned',
    'Stopping',
    'Stopped'
  ]
};

export default class AZSelectorTable extends Component {
  constructor(props) {
    super(props);
    if (this.props.type === 'Async' && isNonEmptyObject(this.props.universe.currentUniverse.data)) {
      if (
        isDefinedNotNull(
          getReadOnlyCluster(this.props.universe.currentUniverse.data.universeDetails.clusters)
        )
      ) {
        this.state = { azItemState: [], isReadOnlyExists: true };
      } else {
        this.state = { azItemState: [], isReadOnlyExists: false };
      }
    } else {
      this.state = { azItemState: [] };
    }
  }

  static propTypes = {
    universe: PropTypes.object
  };

  resetAZSelectionConfig = () => {
    const {
      universe: { universeConfigTemplate },
      clusterType
    } = this.props;
    const clusters = _.clone(universeConfigTemplate.data.clusters);
    const currentTemplate = _.clone(universeConfigTemplate.data, true);
    if (isNonEmptyArray(clusters)) {
      currentTemplate.clusters.forEach(function (cluster, idx) {
        if (cluster.clusterType.toLowerCase() === clusterType) {
          delete currentTemplate.clusters[idx]['placementInfo'];
        }
      });
    }
    currentTemplate.resetAZConfig = true;
    currentTemplate.currentClusterType = clusterType.toUpperCase();
    if (isEmptyObject(this.props.universe.currentUniverse.data)) {
      currentTemplate.clusterOperation = 'CREATE';
    } else {
      currentTemplate.clusterOperation = 'EDIT';
    }
    this.props.submitConfigureUniverse(currentTemplate);
  };

  handleAZChange(oldZoneId, newZoneId) {
    const {
      universe: { universeConfigTemplate }
    } = this.props;
    const currentAZState = [...this.state.azItemState];
    const universeTemplate = _.clone(universeConfigTemplate.data);
    if (!currentAZState.some((azItem) => azItem.value === newZoneId)) {
      const item = currentAZState.find(item => item.value === oldZoneId);
      item.value = newZoneId;
      this.updatePlacementInfo(currentAZState, universeTemplate);
    }
  }

  handleAZNodeCountChange(zoneId, value) {
    const {
      universe: { currentPlacementStatus, universeConfigTemplate }
    } = this.props;
    const universeTemplate = _.clone(universeConfigTemplate.data);
    const currentAZState = [...this.state.azItemState];
    const replicationFactor = currentPlacementStatus.replicationFactor;
    const item = currentAZState.find(item => item.value === zoneId);
    const originalValue = item.count;
    let totalNumNodes = 0;
    currentAZState.forEach((az) => {
      if (az.value !== item.value) {
        totalNumNodes += az.count;
      } else {
        totalNumNodes += value;
      }
    });

    if (totalNumNodes >= replicationFactor) {
      item.count = value;
      this.updatePlacementInfo(currentAZState, universeTemplate);
    } else {
      item.count = originalValue;
      this.setState({ azItemState: currentAZState });
    }
  }

  handleAffinitizedZoneChange(zoneId) {
    const {
      universe: { universeConfigTemplate }
    } = this.props;
    const currentAZState = [...this.state.azItemState];
    const universeTemplate = _.clone(universeConfigTemplate.data);
    const item = currentAZState.find(item => item.value === zoneId);
    item.isAffinitized = !item.isAffinitized;
    this.updatePlacementInfo(currentAZState, universeTemplate);
  }

  // Method takes in the cluster object that is being modified
  // and returns a list of objects each containing the azUUID, azName and count in the record.
  getZonesWithCounts = (cluster) => {
    const {
      cloud: { regions }
    } = this.props;
    return regions.data
      .filter((region) => {
        return cluster.userIntent.regionList.includes(region.uuid);
      })
      .reduce((az, region) => {
        return az.concat(region.zones);
      }, []);
  };

  updatePlacementInfo = (currentAZState, universeConfigTemplate) => {
    const {
      universe: { currentUniverse },
      cloud,
      numNodesChangedViaAzList,
      currentProvider,
      maxNumNodes,
      minNumNodes,
      clusterType
    } = this.props;
    this.setState({ azItemState: currentAZState });
    let totalNodesInConfig = 0;
    currentAZState.forEach(function (item) {
      totalNodesInConfig += item.count;
    });
    numNodesChangedViaAzList(totalNodesInConfig);

    const cluster = clusterType === 'primary'
      ? getPrimaryCluster(universeConfigTemplate.clusters)
      : getReadOnlyCluster(universeConfigTemplate.clusters);

    if (
      (currentProvider.code !== 'onprem' || totalNodesInConfig <= maxNumNodes) &&
      totalNodesInConfig >= minNumNodes &&
      isNonEmptyObject(cluster)
    ) {
      const newPlacementInfo = _.cloneDeep(cluster.placementInfo, true);
      const newRegionList = [];
      cloud.regions.data.forEach(function (regionItem) {
        const newAzList = [];
        let zoneFoundInRegion = false;
        let newRegion = null;
        newPlacementInfo.cloudList[0].regionList.forEach(function (region) {
          if (region.uuid === regionItem.uuid) {
            newRegion = region;
          }
        });

        regionItem.zones.forEach(function (zoneItem) {
          let replicationFactor = 1;
          if (newRegion !== null) {
            newRegion.azList.forEach(function (az) {
              if (az.uuid === zoneItem.uuid) {
                replicationFactor = az.replicationFactor;
              }
            });
          }

          currentAZState.forEach(function (azItem) {
            if (zoneItem.uuid === azItem.value) {
              zoneFoundInRegion = true;
              newAzList.push({
                uuid: zoneItem.uuid,
                replicationFactor: replicationFactor,
                subnet: zoneItem.subnet,
                name: zoneItem.name,
                numNodesInAZ: azItem.count,
                isAffinitized: azItem.isAffinitized
              });
            }
          });
        });
        if (zoneFoundInRegion) {
          newRegionList.push({
            uuid: regionItem.uuid,
            code: regionItem.code,
            name: regionItem.name,
            azList: newAzList
          });
        }
      });
      newPlacementInfo.cloudList[0].regionList = newRegionList;
      const newTaskParams = _.cloneDeep(universeConfigTemplate, true);
      if (isNonEmptyArray(newTaskParams.clusters)) {
        newTaskParams.clusters.forEach((cluster) => {
          if (clusterType === 'primary' && cluster.clusterType === 'PRIMARY') {
            cluster.placementInfo = newPlacementInfo;
            cluster.userIntent.numNodes = totalNodesInConfig;
          } else if (clusterType === 'async' && cluster.clusterType === 'ASYNC') {
            cluster.placementInfo = newPlacementInfo;
            cluster.userIntent.numNodes = totalNodesInConfig;
          }
        });
      }
      if (isEmptyObject(currentUniverse.data)) {
        newTaskParams.currentClusterType = clusterType.toUpperCase();
        newTaskParams.clusterOperation = 'CREATE';
        newTaskParams.resetAZConfig = false;
        this.props.submitConfigureUniverse(newTaskParams);
      } else if (!areUniverseConfigsEqual(newTaskParams, currentUniverse.data.universeDetails)) {
        newTaskParams.universeUUID = currentUniverse.data.universeUUID;
        newTaskParams.currentClusterType = clusterType.toUpperCase();
        newTaskParams.clusterOperation = 'EDIT';
        newTaskParams.expectedUniverseVersion = currentUniverse.data.version;
        newTaskParams.userAZSelected = true;
        newTaskParams.resetAZConfig = false;
        if (
          isNonEmptyObject(
            getClusterByType(currentUniverse.data.universeDetails.clusters, clusterType)
          )
        ) {
          if (
            _.isEqual(
              getClusterByType(newTaskParams.clusters, clusterType).placementInfo,
              getClusterByType(currentUniverse.data.universeDetails.clusters, clusterType)
                .placementInfo
            )
          ) {
            newTaskParams.resetAZConfig = true;
          }
        }
        this.props.submitConfigureUniverse(newTaskParams);
      } else {
        const placementStatusObject = {
          error: {
            type: 'noFieldsChanged',
            numNodes: totalNodesInConfig,
            maxNumNodes: maxNumNodes
          }
        };
        this.props.setPlacementStatus(placementStatusObject);
      }
    } else if (totalNodesInConfig > maxNumNodes && currentProvider.code === 'onprem') {
      const placementStatusObject = {
        error: {
          type: 'notEnoughNodesConfigured',
          numNodes: totalNodesInConfig,
          maxNumNodes: maxNumNodes
        }
      };
      this.props.setPlacementStatus(placementStatusObject);
    } else {
      const placementStatusObject = {
        error: {
          type: 'notEnoughNodes',
          numNodes: totalNodesInConfig,
          maxNumNodes: maxNumNodes
        }
      };
      this.props.setPlacementStatus(placementStatusObject);
    }
  };

  getGroupWithCounts = (universeConfigTemplate) => {
    const {
      cloud: { regions },
      clusterType
    } = this.props;
    const uniConfigArray = [];
    let cluster = null;
    if (isNonEmptyObject(universeConfigTemplate)) {
      if (clusterType === 'primary') {
        cluster = getPrimaryCluster(universeConfigTemplate.clusters);
      } else {
        cluster = getReadOnlyCluster(universeConfigTemplate.clusters);
      }

      if (isNonEmptyObject(universeConfigTemplate.nodeDetailsSet) && isNonEmptyObject(cluster)) {
        universeConfigTemplate.nodeDetailsSet
          .filter(
            (nodeItem) =>
              nodeItem.placementUuid === cluster.uuid &&
              (nodeItem.state === 'ToBeAdded' || nodeItem.state === 'Live') &&
              nodeItem.isTserver
          )
          .forEach((nodeItem) => {
            if (nodeStates.activeStates.includes(nodeItem.state)) {
              const targetNode = uniConfigArray.find((n) => n.value === nodeItem.azUuid);
              if (targetNode) {
                targetNode.count++;
              } else {
                uniConfigArray.push({ value: nodeItem.azUuid, count: 1 });
              }
            }
          });
      }
    }
    let groupsArray = [];
    const uniqueRegions = [];
    if (
      isNonEmptyObject(cluster) &&
      isNonEmptyObject(cluster.placementInfo) &&
      isNonEmptyArray(cluster.placementInfo.cloudList) &&
      isNonEmptyArray(cluster.placementInfo.cloudList[0].regionList)
    ) {
      cluster.placementInfo.cloudList[0].regionList.forEach((regionItem) => {
        regionItem.azList.forEach((azItem) => {
          uniConfigArray.forEach((configArrayItem) => {
            if (configArrayItem.value === azItem.uuid) {
              groupsArray.push({
                value: azItem.uuid,
                count: configArrayItem.count,
                isAffinitized: azItem.isAffinitized === undefined ? true : azItem.isAffinitized
              });
              if (uniqueRegions.indexOf(regionItem.uuid) === -1) {
                uniqueRegions.push(regionItem.uuid);
              }
            }
          });
        });
      });
    }

    const clusters = universeConfigTemplate.clusters;
    if (isNonEmptyArray(clusters)) {
      let azListForSelectedRegions = [];
      const sortedGroupArray = [];
      const currentCluster = getClusterByType(clusters, clusterType);
      if (
        isNonEmptyObject(currentCluster) &&
        isNonEmptyObject(currentCluster.userIntent) &&
        isNonEmptyArray(currentCluster.userIntent.regionList) &&
        isNonEmptyArray(regions.data)
      ) {
        azListForSelectedRegions = this.getZonesWithCounts(currentCluster);
      }
      const sortedAZListForSelectedRegions = azListForSelectedRegions.sort((a, b) => {
        return a.code > b.code ? 1 : -1;
      });
      sortedAZListForSelectedRegions.forEach((azListRegionItem) => {
        const currentazItem = groupsArray.find((a) => a.value === azListRegionItem.uuid);
        if (isNonEmptyObject(currentazItem)) {
          sortedGroupArray.push(currentazItem);
        }
      });
      if (isNonEmptyArray(sortedGroupArray)) {
        groupsArray = sortedGroupArray;
      }
    }

    return {
      groups: groupsArray,
      uniqueRegions: uniqueRegions.length,
      uniqueAzs: [...new Set(groupsArray.map((item) => item.value))].length
    };
  };

  UNSAFE_componentWillMount() {
    const {
      universe: { currentUniverse, universeConfigTemplate },
      type,
      clusterType
    } = this.props;
    const currentCluster = getPromiseState(universeConfigTemplate).isSuccess()
      ? getClusterByType(universeConfigTemplate.data.clusters, clusterType)
      : {};
    // Set AZ Groups when switching back to a cluster tab
    if (isNonEmptyObject(currentCluster)) {
      const azGroups = this.getGroupWithCounts(universeConfigTemplate.data).groups;
      this.setState({ azItemState: azGroups });
    }
    if (
      (type === 'Edit' || (type === 'Async' && this.state.isReadOnlyExists)) &&
      isNonEmptyObject(currentUniverse)
    ) {
      const azGroups = this.getGroupWithCounts(currentUniverse.data.universeDetails).groups;
      this.setState({ azItemState: azGroups });
    }
  }

  UNSAFE_componentWillReceiveProps(nextProps) {
    const {
      universe: { universeConfigTemplate, currentUniverse },
      clusterType,
      type
    } = nextProps;
    if (getPromiseState(universeConfigTemplate).isSuccess()) {
      const placementInfo = this.getGroupWithCounts(universeConfigTemplate.data);
      const azGroups = placementInfo.groups;
      if (
        !areUniverseConfigsEqual(
          this.props.universe.universeConfigTemplate.data,
          universeConfigTemplate.data
        )
      ) {
        this.setState({ azItemState: azGroups });
      }
      const currentCluster = getPromiseState(currentUniverse).isSuccess()
        ? getClusterByType(currentUniverse.data.universeDetails.clusters, clusterType)
        : {};
      const configTemplateCurrentCluster = isNonEmptyObject(universeConfigTemplate.data)
        ? getClusterByType(universeConfigTemplate.data.clusters, clusterType)
        : null;
      if (
        isNonEmptyObject(configTemplateCurrentCluster) &&
        isNonEmptyObject(configTemplateCurrentCluster.placementInfo) &&
        !_.isEqual(universeConfigTemplate, this.props.universe.universeConfigTemplate)
      ) {
        const uniqueAZs = [...new Set(azGroups.map((item) => item.value))];
        const totalNodes = placementInfo.groups.reduce((acc, obj) => acc + obj.count, 0);
        const placementStatusObject = {
          numUniqueRegions: placementInfo.uniqueRegions,
          numUniqueAzs: placementInfo.uniqueAzs,
          replicationFactor: configTemplateCurrentCluster.userIntent.replicationFactor
        };
        if (
          isNonEmptyObject(uniqueAZs) &&
          (type === 'Create' ||
            (type === 'Async' && !isDefinedNotNull(currentCluster)) ||
            currentCluster.userIntent.numNodes !== totalNodes ||
            !_.isEqual(totalNodes, this.props.universe.currentPlacementStatus))
        ) {
          this.props.setPlacementStatus(placementStatusObject);
        }
      }
    }
  }

  render() {
    const {
      universe: { universeConfigTemplate },
      cloud: { regions },
      clusterType
    } = this.props;
    const isReadOnlyTab = clusterType === 'async';
    const clusters = _.get(universeConfigTemplate, 'data.clusters', []);
    const activeCluster =
      clusterType === 'primary' ? getPrimaryCluster(clusters) : getReadOnlyCluster(clusters);
    const replicationFactor = _.get(activeCluster, 'userIntent.replicationFactor', 3);
    let azListForSelectedRegions = [];

    let currentCluster = null;

    if (
      isNonEmptyObject(universeConfigTemplate.data) &&
      isNonEmptyArray(universeConfigTemplate.data.clusters)
    ) {
      currentCluster = getClusterByType(universeConfigTemplate.data.clusters, clusterType);
    }

    if (
      isNonEmptyObject(currentCluster) &&
      isNonEmptyObject(currentCluster.userIntent) &&
      isNonEmptyArray(currentCluster.userIntent.regionList) &&
      isNonEmptyArray(regions.data)
    ) {
      azListForSelectedRegions = this.getZonesWithCounts(currentCluster);
    }
    let azListOptions = <option />;
    if (isNonEmptyArray(azListForSelectedRegions)) {
      azListOptions = azListForSelectedRegions.map((azItem) => (
        <option key={azItem.uuid} value={azItem.uuid}>
          {azItem.code}
        </option>
      ));
    }

    const addNewAZField = () => {
      const unusedAZList = [...azListForSelectedRegions];
      this.state.azItemState.forEach((azGroup) => {
        for (let i = 0; i < unusedAZList.length; i++) {
          if (unusedAZList[i].uuid === azGroup.value) {
            unusedAZList.splice(i, 1);
            break;
          }
        }
      });

      if (unusedAZList.length) {
        const count = this.props.type === "Edit" ? 1 : 0;
        const newAZState = [
          ...this.state.azItemState,
          {
            count: count,
            value: unusedAZList[0].uuid,
            isAffinitized: true
          }
        ];
        this.setState({ azItemState: newAZState });
        const universeTemplate = _.clone(universeConfigTemplate.data);
        this.updatePlacementInfo(newAZState, universeTemplate);
      }
    };

    let azList = [];
    if (isNonEmptyArray(this.state.azItemState) && isNonEmptyArray(azListForSelectedRegions)) {
      azList = this.state.azItemState.map((azGroupItem) => (
        <FlexContainer key={azGroupItem.value}>
          <FlexGrow power={1}>
            <Row>
              <Col xs={8}>
                <Field
                  name={`select${azGroupItem.value}`}
                  component={YBControlledSelect}
                  options={azListOptions}
                  selectVal={azGroupItem.value}
                  onInputChanged={(event) => this.handleAZChange(azGroupItem.value, event.target.value)}
                />
              </Col>
              <Col xs={4}>
                <Field
                  name={`nodes${azGroupItem.value}`}
                  component={YBControlledNumericInput}
                  val={azGroupItem.count}
                  className={getPromiseState(universeConfigTemplate).isLoading() ? 'readonly' : ''}
                  onInputChanged={(value) => this.handleAZNodeCountChange(azGroupItem.value, value)}
                />
              </Col>
            </Row>
          </FlexGrow>
          {!isReadOnlyTab && (
            <FlexShrink power={0} key={azGroupItem.value} className="form-right-control">
              <Field
                name={`affinitized${azGroupItem.value}`}
                component={YBCheckBox}
                checkState={azGroupItem.isAffinitized}
                onClick={() => this.handleAffinitizedZoneChange(azGroupItem.value)}
              />
            </FlexShrink>
          )}
        </FlexContainer>
      ));
      return (
        <div className={'az-table-container form-field-grid'}>
          <div className="az-selector-label">
            <span className="az-selector-reset" onClick={this.resetAZSelectionConfig}>
              Reset Config
            </span>
            <h4>Availability Zones</h4>
          </div>
          <FlexContainer>
            <FlexGrow power={1}>
              <Row>
                <Col xs={8}>
                  <label>Name</label>
                </Col>
                <Col xs={4}>
                  <label>{this.props.isKubernetesUniverse ? 'Pods' : 'Nodes'}</label>
                </Col>
              </Row>
            </FlexGrow>
            {!isReadOnlyTab && (
              <FlexShrink power={0} className="form-right-control">
                <label>Preferred</label>
              </FlexShrink>
            )}
          </FlexContainer>
          {azList}
          {isNonEmptyArray(azListForSelectedRegions) &&
            azList.length < replicationFactor &&
            azList.length < azListForSelectedRegions.length && (
            <Row>
              <Col xs={4}>
                <YBButton
                  btnText="Add Zone"
                  btnIcon="fa fa-plus"
                  btnClass={'btn btn-orange universe-form-add-az-btn'}
                  onClick={addNewAZField}
                />
              </Col>
            </Row>
          )}
        </div>
      );
    }
    return <span />;
  }
}
