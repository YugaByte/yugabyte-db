// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import PropTypes from 'prop-types';
import { Grid } from 'react-bootstrap';
import { change, Fields } from 'redux-form';
import {browserHistory, withRouter} from 'react-router';
import _ from 'lodash';
import { isNonEmptyObject, isDefinedNotNull, isNonEmptyString, isNonEmptyArray } from '../../../utils/ObjectUtils';
import { YBButton, YBModal } from '../../../components/common/forms/fields';
import { UniverseResources } from '../UniverseResources';
import { FlexContainer, FlexShrink } from '../../common/flexbox/YBFlexBox';
import './UniverseForm.scss';
import ClusterFields from './ClusterFields';
import { getPrimaryCluster, getReadOnlyCluster, getClusterByType } from "../../../utils/UniverseUtils";
import { DeleteUniverseContainer } from '../../universes';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { isEmptyObject } from '../../../utils/ObjectUtils';
import pluralize from 'pluralize';

const initialState = {
  instanceTypeSelected: '',
  nodeSetViaAZList: false,
  placementInfo: {},
  currentView: 'Primary',
};

class UniverseForm extends Component {
  static propTypes = {
    type: PropTypes.oneOf(['Async', 'Edit', 'Create']).isRequired,
  };

  constructor(props, context) {
    super(props);
    this.createUniverse = this.createUniverse.bind(this);
    this.editUniverse = this.editUniverse.bind(this);
    this.handleCancelButtonClick = this.handleCancelButtonClick.bind(this);
    this.handleSubmitButtonClick = this.handleSubmitButtonClick.bind(this);
    this.getFormPayload = this.getFormPayload.bind(this);
    this.configureReadOnlyCluster = this.configureReadOnlyCluster.bind(this);
    this.configurePrimaryCluster = this.configurePrimaryCluster.bind(this);
    this.editReadReplica = this.editReadReplica.bind(this);
    this.addReadReplica = this.addReadReplica.bind(this);
    this.updateFormField = this.updateFormField.bind(this);
    this.toggleDisableSubmit = this.toggleDisableSubmit.bind(this);
    this.getCurrentProvider = this.getCurrentProvider.bind(this);
    this.state = {
      ...initialState,
      hasFieldChanged: true,
      disableSubmit: false,
      currentView: props.type === "Async" ? 'Async' : 'Primary'
    };
  }

  toggleDisableSubmit = (value) => {
    this.setState({ disableSubmit: value });
  }

  handleHasFieldChanged = (hasFieldChanged) => {
    if (hasFieldChanged === this.state.hasFieldChanged) this.setState({hasFieldChanged: !hasFieldChanged});
  }

  getCurrentProvider(providerUUID) {
    return this.props.cloud.providers.data.find((provider) => provider.uuid === providerUUID);
  }

  updateFormField = (fieldName, fieldValue) => {
    this.props.dispatch(change("UniverseForm", fieldName, fieldValue));
  }

  configureReadOnlyCluster = () => {
    this.setState({currentView: 'Async'});
  }

  configurePrimaryCluster = () => {
    this.setState({currentView: 'Primary'});
  }

  transitionToDefaultRoute = () => {
    const { universe: { currentUniverse: { data: { universeUUID }}}} = this.props;
    if (this.props.type === "Create") {
      if (this.context.prevPath) {
        browserHistory.push(this.context.prevPath);
      } else {
        browserHistory.push("/universes");
      }
    } else {
      if (this.props.location && this.props.location.pathname) {
        this.props.fetchCurrentUniverse(universeUUID);
        browserHistory.push(`/universes/${universeUUID}`);
      }
    }
  }

  handleCancelButtonClick = () => {
    this.setState(initialState);
    this.transitionToDefaultRoute();
    this.props.reset();
  }

  handleSubmitButtonClick = () => {
    const { type } = this.props;
    setTimeout(this.props.fetchCustomerTasks, 2000);
    if (type === "Create") {
      this.createUniverse().then(() => {
        this.transitionToDefaultRoute();
      });
    } else if (type === "Async") {
      const { universe: { currentUniverse: { data: { universeDetails }}}} = this.props;
      const readOnlyCluster = universeDetails && getReadOnlyCluster(universeDetails.clusters);
      if (isNonEmptyObject(readOnlyCluster)) {
        this.editReadReplica();
      } else {
        this.addReadReplica();
      }
      this.transitionToDefaultRoute();
    } else {
      this.editUniverse();
      this.transitionToDefaultRoute();
    }
  }

  getCurrentUserIntent = (clusterType) => {
    const {formValues } = this.props;
    if (formValues[clusterType]) {
      const intent = {
        universeName: formValues[clusterType].universeName,
        numNodes: formValues[clusterType].numNodes,
        provider: formValues[clusterType].provider,
        providerType: this.getCurrentProvider(formValues[clusterType].provider) ?
          this.getCurrentProvider(formValues[clusterType].provider).code :
          null,
        regionList: formValues[clusterType].regionList.map((a)=>(a.value)),
        instanceType: formValues[clusterType].instanceType,
        ybSoftwareVersion: formValues[clusterType].ybSoftwareVersion,
        replicationFactor: formValues[clusterType].replicationFactor,
        deviceInfo: {
          volumeSize: formValues[clusterType].volumeSize,
          numVolumes: formValues[clusterType].numVolumes,
          diskIops: formValues[clusterType].diskIops,
          mountPoints: formValues[clusterType].mountPoints,
          storageType: formValues[clusterType].storageType
        },
        accessKeyCode: formValues[clusterType].accessKeyCode,
        instanceTags: formValues[clusterType].instanceTags,
        useTimeSync: formValues[clusterType].useTimeSync,
        assignPublicIP: formValues[clusterType].assignPublicIP,
        enableYSQL: formValues[clusterType].enableYSQL,
        enableNodeToNodeEncrypt: formValues[clusterType].enableNodeToNodeEncrypt,
        enableClientToNodeEncrypt: formValues[clusterType].enableClientToNodeEncrypt
      };

      if (isNonEmptyObject(formValues[clusterType].masterGFlags)) {
        intent["masterGFlags"] = formValues[clusterType].masterGFlags;
      }
      if (isNonEmptyObject(formValues[clusterType].tserverGFlags)) {
        intent["tserverGFlags"] = formValues[clusterType].tserverGFlags;
      }
      return intent;
    }
  };

  updateTaskParams = (universeTaskParams, userIntent, clusterType, isEdit) => {
    const cluster = getClusterByType(universeTaskParams.clusters, clusterType);
    universeTaskParams.currentClusterType = clusterType.toUpperCase();

    if (isDefinedNotNull(cluster)) {
      cluster.userIntent = userIntent;
    } else {
      if (isEmptyObject(universeTaskParams.clusters)) {
        universeTaskParams.clusters = [];
      }
      universeTaskParams.clusters.push({
        clusterType: clusterType.toUpperCase(),
        userIntent: userIntent
      });
    }
    universeTaskParams.clusterOperation = isEdit ? "EDIT": "CREATE";
  }

  createUniverse = () => {
    const { formValues, universe } = this.props;
    if (isNonEmptyObject(formValues['primary'])) {
      const { universeConfigTemplate } = universe;
      const primaryCluster = getPrimaryCluster(universeConfigTemplate.data.clusters);
      if (formValues['primary'].universeName !== primaryCluster.userIntent.universeName) {
        // Universe name is out of sync, send a configure call
        const universeTaskParams = _.cloneDeep(universeConfigTemplate.data);
        this.updateTaskParams(universeTaskParams, this.getCurrentUserIntent('primary'), 'primary', false);
        return this.props.submitConfigureUniverse(universeTaskParams).then(response => {
          return this.props.submitCreateUniverse(_.merge(this.getFormPayload(), response.payload.data));
        });
      }
      return this.props.submitCreateUniverse(this.getFormPayload());
    }
  }

  editUniverse = () => {
    const {universe: {currentUniverse: {data: {universeUUID}}}} = this.props;
    this.props.submitEditUniverse(this.getFormPayload(), universeUUID);
  }

  addReadReplica = () => {
    const {universe: {currentUniverse: {data: {universeUUID}}}} = this.props;
    this.props.submitAddUniverseReadReplica(this.getFormPayload(), universeUUID);
  }

  editReadReplica = () => {
    const {universe: {currentUniverse: {data: {universeUUID}}}} = this.props;
    this.props.submitEditUniverseReadReplica(this.getFormPayload(), universeUUID);
  }

  UNSAFE_componentWillMount() {
    this.props.resetConfig();
    this.setState({editNotAllowed: true});
  }

  componentWillUnmount() {
    this.props.resetConfig();
  }

  UNSAFE_componentWillUpdate(newProps) {
    if (newProps.universe.formSubmitSuccess) {
      this.props.reset();
    }
  }

  // For Async clusters, we need to fetch the universe name from the
  // primary cluster metadata
  getUniverseName = () => {
    const { formValues, universe } = this.props;

    if (isNonEmptyObject(formValues['primary'])) {
      return formValues['primary'].universeName;
    }

    const { currentUniverse: { data: { universeDetails }}} = universe;
    if (isNonEmptyObject(universeDetails)) {
      const primaryCluster = getPrimaryCluster(universeDetails.clusters);
      return primaryCluster.userIntent.universeName;
    }
    // We shouldn't get here!!!
    return null;
  }

  getYSQLstate = () => {
    const { formValues, universe } = this.props;

    if (isNonEmptyObject(formValues['primary'])) {
      return formValues['primary'].enableYSQL;
    }

    const { currentUniverse: { data: { universeDetails }}} = universe;
    if (isNonEmptyObject(universeDetails)) {
      const primaryCluster = getPrimaryCluster(universeDetails.clusters);
      return primaryCluster.userIntent.enableYSQL;
    }
    // We shouldn't get here!!!
    return null;
  }

  getFormPayload = () => {
    const { formValues, universe, type } = this.props;
    const { universeConfigTemplate, currentUniverse: { data: { universeDetails }}} = universe;
    const submitPayload = {..._.clone(universeConfigTemplate.data, true)};
    const self = this;
    const getIntentValues = function (clusterType) {
      if (!isNonEmptyObject(formValues[clusterType]) ||
          !isNonEmptyString(formValues[clusterType].provider) ||
          !isNonEmptyArray(formValues[clusterType].regionList)) {
        return null;
      }
      const clusterIntent = {
        regionList: formValues[clusterType].regionList.map(function (item) {
          return item.value;
        }),
        // We only have universe name field captured at primary form
        universeName: self.getUniverseName().trim(),
        provider: formValues[clusterType].provider,
        assignPublicIP: formValues[clusterType].assignPublicIP,
        useTimeSync: formValues[clusterType].useTimeSync,
        enableYSQL: self.getYSQLstate(),
        enableNodeToNodeEncrypt: formValues[clusterType].enableNodeToNodeEncrypt,
        enableClientToNodeEncrypt: formValues[clusterType].enableClientToNodeEncrypt,
        awsArnString: formValues[clusterType].awsArnString,
        providerType: self.getCurrentProvider(formValues[clusterType].provider).code,
        instanceType: formValues[clusterType].instanceType,
        numNodes: formValues[clusterType].numNodes,
        accessKeyCode: formValues[clusterType].accessKeyCode,
        replicationFactor: formValues[clusterType].replicationFactor,
        ybSoftwareVersion: formValues[clusterType].ybSoftwareVersion,
        deviceInfo: {
          volumeSize: formValues[clusterType].volumeSize,
          numVolumes: formValues[clusterType].numVolumes,
          diskIops: formValues[clusterType].diskIops,
          mountPoints: formValues[clusterType].mountPoints,
          storageType: formValues[clusterType].storageType,
        },
      };
      const currentProvider = self.getCurrentProvider(formValues[clusterType].provider).code;
      if (clusterType === "primary") {
        clusterIntent.masterGFlags = formValues.primary.masterGFlags.filter((masterFlag) => {
          return isNonEmptyString(masterFlag.name) && isNonEmptyString(masterFlag.value);
        }).map((masterFlag) => {
          return {name: masterFlag.name, value: masterFlag.value};
        });
        clusterIntent.tserverGFlags = formValues.primary.tserverGFlags.filter((tserverFlag) => {
          return isNonEmptyString(tserverFlag.name) && isNonEmptyString(tserverFlag.value);
        }).map((tserverFlag) => {
          return {name: tserverFlag.name, value: tserverFlag.value.trim()};
        });

        if (currentProvider==='aws') {
          clusterIntent.instanceTags = formValues.primary.instanceTags.filter((userTag) => {
            return isNonEmptyString(userTag.name) && isNonEmptyString(userTag.value);
          }).map((userTag) => {
            return {name: userTag.name, value: userTag.value.trim()};
          });
        }
      } else {
        if (isDefinedNotNull(formValues.primary)) {
          clusterIntent.tserverGFlags = formValues.primary.tserverGFlags.filter((tserverFlag) => {
            return isNonEmptyString(tserverFlag.name) && isNonEmptyString(tserverFlag.value);
          }).map((tserverFlag) => {
            return {name: tserverFlag.name, value: tserverFlag.value.trim()};
          });
        } else {
          const existingTserverGFlags = getPrimaryCluster(universeDetails.clusters).userIntent.tserverGFlags;
          const tserverGFlags = [];
          Object.entries(existingTserverGFlags).forEach(
            ([key, value]) => tserverGFlags.push({'name': key, 'value': value.trim()})
          );
          clusterIntent.tserverGFlags = tserverGFlags;
        }
      }
      return clusterIntent;
    };

    let asyncClusterFound = false;
    if (isNonEmptyArray(submitPayload.clusters)) {
      submitPayload.clusters.forEach(function (cluster, idx, arr) {
        if (cluster.clusterType === "PRIMARY" && isNonEmptyObject(getIntentValues("primary"))) {
          submitPayload.clusters[idx].userIntent = getIntentValues("primary");
          const tlsEnabled = formValues['primary'].enableClientToNodeEncrypt || formValues['primary'].enableNodeToNodeEncrypt;
          if (formValues['primary'].tlsCertificateId && tlsEnabled) {
            submitPayload.rootCA = formValues['primary'].tlsCertificateId;
          }

          const kmsConfigUUID = formValues['primary'].selectEncryptionAtRestConfig;

          // Default universe key parameters... we can eventually expose this in the UI to
          // allow users to configure their universe keys at a more granular level
          submitPayload.encryptionAtRestConfig = {
            "key_op": formValues['primary'].enableEncryptionAtRest ? "ENABLE" : "UNDEFINED"
          };

          submitPayload.communicationPorts = {
            "masterHttpPort": formValues['primary'].masterHttpPort,
            "masterRpcPort": formValues['primary'].masterRpcPort,
            "tserverHttpPort": formValues['primary'].tserverHttpPort,
            "tserverRpcPort": formValues['primary'].tserverRpcPort,
            "redisServerHttpPort": formValues['primary'].redisHttpPort,
            "redisServerRpcPort": formValues['primary'].redisRpcPort,
            "yqlServerHttpPort": formValues['primary'].yqlHttpPort,
            "yqlServerRpcPort": formValues['primary'].yqlRpcPort,
            "ysqlServerHttpPort": formValues['primary'].ysqlHttpPort,
            "ysqlServerRpcPort": formValues['primary'].ysqlRpcPort,
            "nodeExporterPort": formValues['primary'].nodeExporterPort
          };

          submitPayload.extraDependencies = {
            "installNodeExporter": formValues['primary'].installNodeExporter
          };

          // Ensure a configuration was actually selected
          if (kmsConfigUUID !== null) {
            submitPayload.encryptionAtRestConfig['configUUID'] = kmsConfigUUID;
          }
        }
        if (cluster.clusterType === "ASYNC" && isNonEmptyObject(getIntentValues("async"))) {
          asyncClusterFound = true;
          submitPayload.clusters[idx].userIntent = getIntentValues("async");
        }
      });

      // If async cluster array is not set then set it
      if (isNonEmptyObject(getIntentValues("async")) && !asyncClusterFound) {
        submitPayload.clusters.push({
          clusterType: "ASYNC",
          userIntent: getIntentValues("async")
        });
      }
    } else {
      submitPayload.clusters = [
        {
          clusterType: "PRIMARY",
          userIntent: getIntentValues("primary")
        },
        {
          clusterType: "ASYNC",
          userIntent: getIntentValues("async")
        }
      ];
    }

    submitPayload.clusters = submitPayload.clusters.filter((c)=>(c.userIntent !== null));
    // filter clusters array if configuring(adding only) Read Replica due to server side validation
    if (type === "Async") {
      submitPayload.clusters = submitPayload.clusters.filter((c)=>(c.clusterType !== "PRIMARY"));
      if (!isDefinedNotNull(getReadOnlyCluster(universeDetails.clusters))) {
        submitPayload.nodeDetailsSet = submitPayload.nodeDetailsSet.filter((c)=>(c.state === "ToBeAdded"));
      }
    }

    return submitPayload;
  }

  render() {
    const {
      handleSubmit,
      universe,
      universe: { universeConfigTemplate },
      softwareVersions,
      cloud,
      getInstanceTypeListItems,
      submitConfigureUniverse,
      type,
      getRegionListItems,
      resetConfig,
      formValues,
      userCertificates,
      fetchUniverseResources,
      fetchNodeInstanceList,
      showDeleteReadReplicaModal,
      closeModal,
      showFullMoveModal,
      modal: { showModal, visibleModal }
    } = this.props;
    const { disableSubmit, hasFieldChanged } = this.state;
    const createUniverseTitle =
      (<h2 className="content-title">
        <FlexContainer>
          <FlexShrink>
            <div>{this.props.type} universe</div>
          </FlexShrink>
          <FlexShrink className={this.state.currentView === "Primary" ? 'stepper-cell active-stepper-cell' : 'stepper-cell'}>
            1. Primary Cluster
          </FlexShrink>
          <FlexShrink className={this.state.currentView === "Primary" ? 'stepper-cell' : 'stepper-cell active-stepper-cell'}>
            2. Read Replica
          </FlexShrink>
        </FlexContainer>
      </h2>);

    let primaryUniverseName = "";
    if (universe && isDefinedNotNull(universe.currentUniverse.data.universeDetails)) {
      primaryUniverseName = getPrimaryCluster(universe.currentUniverse.data.universeDetails.clusters).userIntent.universeName;
    }

    const pageTitle = (({type}) => {
      if (type === "Async") {
        return <h2 className="content-title">{primaryUniverseName}<span> <i className="fa fa-chevron-right"></i> Configure read replica </span></h2>;
      } else {
        if (type === "Create") {
          return createUniverseTitle;
        } else {
          return <h2 className="content-title">{primaryUniverseName}<span> <i className="fa fa-chevron-right"></i> {this.props.type} Universe </span></h2>;
        }
      }
    })(this.props);

    let clusterForm = <span/>;
    let primaryReplicaBtn = <span/>;
    let asyncReplicaBtn = <span/>;

    if (this.state.currentView === "Async" && type !== "Edit" && type !== "Async") {
      primaryReplicaBtn = <YBButton btnClass="btn btn-default universe-form-submit-btn" btnText={"Back to Primary cluster"} onClick={this.configurePrimaryCluster}/>;
    }

    if (this.state.currentView === "Primary" && type !== "Edit" && type !== "Async") {
      asyncReplicaBtn = <YBButton btnClass="btn btn-default universe-form-submit-btn" btnText={"Configure Read Replica"} onClick={this.configureReadOnlyCluster}/>;
    }

    const {universe: {currentUniverse: {data: {universeDetails}}}, modal } = this.props;
    const readOnlyCluster = universeDetails && getReadOnlyCluster(universeDetails.clusters);

    if (type === "Async") {
      if(readOnlyCluster) {
        asyncReplicaBtn = <YBButton btnClass="btn btn-default universe-form-submit-btn" btnText={"Delete this configuration"} onClick={showDeleteReadReplicaModal}/>;
      } else {
        //asyncReplicaBtn = <YBButton btnClass="btn btn-orange universe-form-submit-btn" btnText={"Add Read Replica"} onClick={this.configureReadOnlyCluster}/>;
      }
    }
    let submitTextLabel = "";
    if (type === "Create") {
      submitTextLabel = "Create";
    } else {
      if (type === "Async") {
        if(readOnlyCluster) {
          submitTextLabel = "Edit Read Replica" ;
        } else {
          submitTextLabel = "Add Read Replica";
        }
      } else {
        submitTextLabel = "Save";
      }
    };

    const clusterProps = {
      universe,
      getRegionListItems,
      getInstanceTypeListItems,
      cloud,
      formValues,
      resetConfig,
      softwareVersions,
      fetchNodeInstanceList,
      userCertificates,
      submitConfigureUniverse,
      type,
      fetchUniverseResources,
      accessKeys: this.props.accessKeys, updateFormField: this.updateFormField,
      setPlacementStatus: this.props.setPlacementStatus,
      getKMSConfigs: this.props.getKMSConfigs,
      fetchUniverseTasks: this.props.fetchUniverseTasks,
      handleHasFieldChanged: this.handleHasFieldChanged,
      toggleDisableSubmit: this.toggleDisableSubmit,
      getCurrentUserIntent: this.getCurrentUserIntent,
      updateTaskParams: this.updateTaskParams,
      reset: this.props.reset, fetchUniverseMetadata: this.props.fetchUniverseMetadata,
      fetchCustomerTasks: this.props.fetchCustomerTasks,
      getExistingUniverseConfiguration: this.props.getExistingUniverseConfiguration,
      fetchCurrentUniverse: this.props.fetchCurrentUniverse, location: this.props.location,
    };

    if (this.state.currentView === "Primary") {
      clusterForm = (<PrimaryClusterFields {...clusterProps}/>);
    } else {
      // show async cluster if view if async
      clusterForm = (<ReadOnlyClusterFields {...clusterProps} />);
    }

    // check nodes if all live nodes is going to be removed (full move)
    const existingPrimaryNodes = getPromiseState(universeConfigTemplate).isSuccess() ? universeConfigTemplate.data.nodeDetailsSet.filter(node => node.nodeName && (type === "Async" ? node.nodeName.includes("readonly") : !node.nodeName.includes("readonly"))) : [];
    const formChangedOrInvalid = hasFieldChanged || disableSubmit;
    let submitControl = (<YBButton
      btnClass="btn btn-orange universe-form-submit-btn"
      btnText={submitTextLabel}
      disabled={true}
    />);
    if ((existingPrimaryNodes.length && existingPrimaryNodes.filter(node => node.state !== "ToBeRemoved").length) ||
        (this.state.currentView === "Primary" && type === "Create") ||
        (this.state.currentView === "Async" && !readOnlyCluster)) {
      submitControl = (<YBButton
        btnClass="btn btn-orange universe-form-submit-btn"
        btnText={submitTextLabel}
        btnType={"submit"}
        disabled={formChangedOrInvalid}
      />);
    } else if (getPromiseState(universeConfigTemplate).isSuccess()) {
      const generateAZConfig = (nodes) => {
        const regionMap = {};
        nodes.forEach((node) => {
          const regionName = node.cloudInfo.region;
          if (!regionMap[regionName]) {
            regionMap[regionName] = {
              region: regionName,
              zones: [{
                az: node.cloudInfo.az,
                count: 1
              }]
            };
          } else {
            // Check if new node is included in zones
            const zoneIndex = regionMap[regionName].zones.findIndex(z => z.az === node.cloudInfo.az);
            if (zoneIndex === -1) {
              // Add new AZ to zone list
              regionMap[regionName].zones.push({
                az: node.cloudInfo.az,
                count: 1
              });
            } else {
              // AZ exists, increment count
              regionMap[regionName].zones[zoneIndex].count += 1;
            }
          }
        });
        return Object.values(regionMap);
      };

      const renderConfig = ({azConfig}) => azConfig.map(
        (region) =>
          (<div className="full-move-config--region">
            <strong>{region.region}</strong>
            {region.zones.map((zone) => <div>{zone.az} - {zone.count} {pluralize('node', zone.count)}</div>)}
          </div>)
      );
      const currentCluster = this.state.currentView === "Primary" ? getPrimaryCluster(universe.currentUniverse.data.universeDetails.clusters) : getReadOnlyCluster(universe.currentUniverse.data.universeDetails.clusters);
      const newCluster = this.state.currentView === "Primary" ? getPrimaryCluster(universeConfigTemplate.data.clusters): getReadOnlyCluster(universeConfigTemplate.data.clusters);
      const placementUuid = newCluster.uuid;
      const oldNodes = universeConfigTemplate.data.nodeDetailsSet
                        .filter(node => node.placementUuid === placementUuid && node.nodeName && node.isTserver);
      const newNodes = universeConfigTemplate.data.nodeDetailsSet
                        .filter(node => node.placementUuid === placementUuid && !node.nodeName);
      const oldConfig = {};
      if (currentCluster) {
        oldConfig.numVolumes =  currentCluster.userIntent.deviceInfo.numVolumes;
        oldConfig.volumeSize = currentCluster.userIntent.deviceInfo.volumeSize;
        oldConfig.instanceType = currentCluster.userIntent.instanceType;

      }

      if (isNonEmptyArray(oldNodes)) {
        oldConfig.azConfig = generateAZConfig(oldNodes);
      }

      const newConfig = {
        instanceType: newCluster.userIntent.instanceType,
        numVolumes: newCluster.userIntent.deviceInfo.numVolumes,
        volumeSize: newCluster.userIntent.deviceInfo.volumeSize
      };

      if (isNonEmptyArray(newNodes)) {
        newConfig.azConfig = generateAZConfig(newNodes);
      }

      submitControl = (<Fragment>
        <YBButton
          onClick={showFullMoveModal}
          btnClass="btn btn-orange universe-form-submit-btn"
          btnText={submitTextLabel}
          disabled={formChangedOrInvalid}
        />
        {visibleModal === "fullMoveModal" && <YBModal visible={showModal && visibleModal === "fullMoveModal"}
              onHide={ closeModal } submitLabel={'Proceed'} cancelLabel={'Cancel'} showCancelButton={true} title={ "Confirm Full Move Update" } onFormSubmit={ handleSubmit(this.handleSubmitButtonClick) } >
          This operation will migrate this universe and all its data to a completely new set of nodes.
          <div className={"full-move-config"}>
            <div className={"text-lightgray full-move-config--general"}>
              <h5>
                Current:
              </h5>
              <b>{oldConfig.instanceType}</b> type<br/>
              <b>{oldConfig.numVolumes}</b> {pluralize('volume', oldConfig.numVolumes)} of <b>{oldConfig.volumeSize}Gb</b> per instance<br/>
            </div>
            <div className={"full-move-config--general"}>
              <h5>
                New:
              </h5>
              <b>{newConfig.instanceType}</b> type<br/>
              <b>{newConfig.numVolumes}</b> {pluralize('volume', newConfig.numVolumes)} of <b>{newConfig.volumeSize}Gb</b> per instance<br/>
            </div>
            <div className={"full-move-config--config text-lightgray"}>
              {renderConfig(oldConfig)}
            </div>
            <div className={"full-move-config--config"}>
              {renderConfig(newConfig)}
            </div>
          </div>
          Would you like to proceed?
        </YBModal>}
      </Fragment>);

    }

    return (
      <Grid id="page-wrapper" fluid={true} className="universe-form-new">
        <DeleteUniverseContainer visible={modal.showModal && modal.visibleModal==="deleteReadReplicaModal"}
          onHide={closeModal} title="Delete Read Replica of " body="Are you sure you want to delete this read replica cluster?" type="async"/>
        {pageTitle}
        <form name="UniverseForm" className="universe-form-container" onSubmit={handleSubmit(this.handleSubmitButtonClick)}>
          {clusterForm}
          <div className="form-action-button-container">
            <UniverseResources resources={universe.universeResourceTemplate.data}>
              <YBButton btnClass="btn btn-default universe-form-submit-btn" btnText="Cancel" onClick={this.handleCancelButtonClick}/>
              {primaryReplicaBtn}
              {asyncReplicaBtn}
              {submitControl}
            </UniverseResources>
            <div className="mobile-view-btn-container">
              <YBButton btnClass="btn btn-default universe-form-submit-btn" btnText="Cancel" onClick={this.handleCancelButtonClick}/>
              {primaryReplicaBtn}
              {asyncReplicaBtn}
              <YBButton btnClass="btn btn-orange universe-form-submit-btn" disabled={disableSubmit} btnText={submitTextLabel} btnType={"submit"} />
            </div>
          </div>
        </form>
      </Grid>
    );
  }
}

UniverseForm.contextTypes = {
  prevPath: PropTypes.string
};

class PrimaryClusterFields extends Component {
  render() {
    return (
      <Fields names={['primary.universeName', 'primary.provider', 'primary.providerType',
        'primary.regionList', 'primary.replicationFactor', 'primary.numNodes',
        'primary.instanceType', 'primary.masterGFlags', 'primary.tserverGFlags',
        'primary.instanceTags', 'primary.ybSoftwareVersion', 'primary.diskIops',
        'primary.numVolumes', 'primary.volumeSize', 'primary.storageType',
        'primary.assignPublicIP', 'primary.useTimeSync', 'primary.enableYSQL',
        'primary.enableNodeToNodeEncrypt', 'primary.enableClientToNodeEncrypt',
        'primary.enableEncryptionAtRest', 'primary.selectEncryptionAtRestConfig'
      ]}
        component={ClusterFields} {...this.props} clusterType={"primary"} />
    );
  }
}

class ReadOnlyClusterFields extends Component {
  render() {
    return (
      <Fields names={['primary.universeName', 'async.provider', 'async.providerType', 'async.regionList', 'async.replicationFactor',
        'async.numNodes', 'async.instanceType', 'async.ybSoftwareVersion', 'async.diskIops',
        'async.numVolumes','async.volumeSize',
        'async.storageType', 'async.assignPublicIP', 'async.useTimeSync', 'async.enableYSQL', 'async.enableNodeToNodeEncrypt',
        'async.enableClientToNodeEncrypt']}
      component={ClusterFields} {...this.props} clusterType={"async"}/>
    );
  }
}

export default withRouter(UniverseForm);
