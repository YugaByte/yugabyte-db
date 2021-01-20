// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { Field, FieldArray } from 'redux-form';
import { browserHistory } from 'react-router';
import _ from 'lodash';
import {
  isDefinedNotNull,
  isNonEmptyObject,
  isNonEmptyString,
  areIntentsEqual,
  isEmptyObject,
  isNonEmptyArray,
  trimSpecialChars,
  normalizeToValidPort
} from '../../../utils/ObjectUtils';
import {
  YBTextInput,
  YBTextInputWithLabel,
  YBSelectWithLabel,
  YBMultiSelectWithLabel,
  YBRadioButtonBarWithLabel,
  YBToggle,
  YBUnControlledNumericInput,
  YBControlledNumericInputWithLabel
} from '../../../components/common/forms/fields';
import { getPromiseState } from '../../../utils/PromiseUtils';
import AZSelectorTable from './AZSelectorTable';
import './UniverseForm.scss';
import AZPlacementInfo from './AZPlacementInfo';
import GFlagArrayComponent from './GFlagArrayComponent';
import {
  getPrimaryCluster,
  getReadOnlyCluster,
  getClusterByType,
  isKubernetesUniverse,
  getPlacementCloud
} from '../../../utils/UniverseUtils';

// Default instance types for each cloud provider
const DEFAULT_INSTANCE_TYPE_MAP = {
  aws: 'c5.large',
  gcp: 'n1-standard-1',
  kubernetes: 'small'
};

// Maps API storage types to UI display options
const API_UI_STORAGE_TYPES = {
  Scratch: 'Local Scratch',
  Persistent: 'Persistent',
  IO1: 'IO1',
  GP2: 'GP2',
  Premium_LRS: 'Premium',
  StandardSSD_LRS: 'Standard',
  UltraSSD_LRS: 'Ultra'
};

const DEFAULT_PORTS = {
  MASTER_HTTP_PORT: 7000,
  MASTER_RPC_PORT: 7100,
  TSERVER_HTTP_PORT: 9000,
  TSERVER_RPC_PORT: 9100,
  YEDIS_HTTP_PORT: 11000,
  YEDIS_RPC_PORT: 6379,
  YQL_HTTP_PORT: 12000,
  YQL_RPC_PORT: 9042,
  YSQL_HTTP_PORT: 13000,
  YSQL_RPC_PORT: 5433
};

const DEFAULT_STORAGE_TYPES = {
  AWS: 'GP2',
  GCP: 'Scratch',
  AZU: 'Premium_LRS'
};

const initialState = {
  universeName: '',
  instanceTypeSelected: '',
  azCheckState: true,
  providerSelected: '',
  regionList: [],
  numNodes: 3,
  nodeSetViaAZList: false,
  isAZUpdating: false,
  replicationFactor: 3,
  deviceInfo: {},
  placementInfo: {},
  ybSoftwareVersion: '',
  gflags: {},
  storageType: DEFAULT_STORAGE_TYPES['AWS'],
  accessKeyCode: 'yugabyte-default',
  // Maximum Number of nodes currently in use OnPrem case
  maxNumNodes: -1,
  assignPublicIP: true,
  hasInstanceTypeChanged: false,
  useTimeSync: false,
  enableYSQL: true,
  enableIPV6: false,
  enableYEDIS: false,
  enableNodeToNodeEncrypt: false,
  enableClientToNodeEncrypt: false,
  enableEncryptionAtRest: false,
  customizePorts: false
};

export default class ClusterFields extends Component {
  constructor(props) {
    super(props);
    this.providerChanged = this.providerChanged.bind(this);
    this.numNodesChanged = this.numNodesChanged.bind(this);
    this.instanceTypeChanged = this.instanceTypeChanged.bind(this);
    this.regionListChanged = this.regionListChanged.bind(this);
    this.getCurrentProvider = this.getCurrentProvider.bind(this);
    this.configureUniverseNodeList = this.configureUniverseNodeList.bind(this);
    this.handleUniverseConfigure = this.handleUniverseConfigure.bind(this);
    this.storageTypeChanged = this.storageTypeChanged.bind(this);
    this.numVolumesChanged = this.numVolumesChanged.bind(this);
    this.volumeSizeChanged = this.volumeSizeChanged.bind(this);
    this.diskIopsChanged = this.diskIopsChanged.bind(this);
    this.setDeviceInfo = this.setDeviceInfo.bind(this);
    this.toggleAssignPublicIP = this.toggleAssignPublicIP.bind(this);
    this.toggleUseTimeSync = this.toggleUseTimeSync.bind(this);
    this.toggleEnableYSQL = this.toggleEnableYSQL.bind(this);
    this.toggleEnableIPV6 = this.toggleEnableIPV6.bind(this);
    this.toggleEnableYEDIS = this.toggleEnableYEDIS.bind(this);
    this.toggleEnableNodeToNodeEncrypt = this.toggleEnableNodeToNodeEncrypt.bind(this);
    this.toggleEnableClientToNodeEncrypt = this.toggleEnableClientToNodeEncrypt.bind(this);
    this.clientToNodeEncryptField = this.clientToNodeEncryptField.bind(this);
    this.toggleEnableEncryptionAtRest = this.toggleEnableEncryptionAtRest.bind(this);
    this.handleAwsArnChange = this.handleAwsArnChange.bind(this);
    this.handleSelectAuthConfig = this.handleSelectAuthConfig.bind(this);
    this.numNodesChangedViaAzList = this.numNodesChangedViaAzList.bind(this);
    this.replicationFactorChanged = this.replicationFactorChanged.bind(this);
    this.softwareVersionChanged = this.softwareVersionChanged.bind(this);
    this.accessKeyChanged = this.accessKeyChanged.bind(this);
    this.hasFieldChanged = this.hasFieldChanged.bind(this);
    this.toggleCustomizePorts = this.toggleCustomizePorts.bind(this);

    this.currentInstanceType = _.get(
      this.props.universe,
      'currentUniverse.data.universeDetails.clusters[0].userIntent.instanceType'
    );

    if (this.props.type === 'Async' && isNonEmptyObject(this.props.universe.currentUniverse.data)) {
      if (
        isDefinedNotNull(
          getReadOnlyCluster(this.props.universe.currentUniverse.data.universeDetails.clusters)
        )
      ) {
        this.state = {
          ...initialState,
          isReadOnlyExists: true,
          editNotAllowed: this.props.editNotAllowed
        };
      } else {
        this.state = { ...initialState, isReadOnlyExists: false, editNotAllowed: false };
      }
    } else {
      this.state = initialState;
    }
  }

  portsCustomized = (communicationPorts) => {
    return (
      communicationPorts.masterHttpPort !== DEFAULT_PORTS.MASTER_HTTP_PORT ||
      communicationPorts.masterRpcPort !== DEFAULT_PORTS.MASTER_RPC_PORT ||
      communicationPorts.tserverHttpPort !== DEFAULT_PORTS.TSERVER_HTTP_PORT ||
      communicationPorts.tserverRpcPort !== DEFAULT_PORTS.TSERVER_RPC_PORT ||
      communicationPorts.redisServerHttpPort !== DEFAULT_PORTS.YEDIS_HTTP_PORT ||
      communicationPorts.redisServerRpcPort !== DEFAULT_PORTS.YEDIS_RPC_PORT ||
      communicationPorts.yqlServerHttpPort !== DEFAULT_PORTS.YQL_HTTP_PORT ||
      communicationPorts.yqlServerRpcPort !== DEFAULT_PORTS.YQL_RPC_PORT ||
      communicationPorts.ysqlServerHttpPort !== DEFAULT_PORTS.YSQL_HTTP_PORT ||
      communicationPorts.ysqlServerRpcPort !== DEFAULT_PORTS.YSQL_RPC_PORT
    );
  };

  UNSAFE_componentWillMount() {
    const { formValues, clusterType, updateFormField, type } = this.props;
    const {
      universe: {
        currentUniverse: {
          data: { universeDetails }
        }
      }
    } = this.props;

    // This prop will help us to get the list of KMS configs.
    this.props.getKMSConfigs();

    // Set default software version in case of create
    if (
      isNonEmptyArray(this.props.softwareVersions) &&
      !isNonEmptyString(this.state.ybSoftwareVersion) &&
      type === 'Create'
    ) {
      this.setState({ ybSoftwareVersion: this.props.softwareVersions[0] });
      updateFormField(`${clusterType}.ybSoftwareVersion`, this.props.softwareVersions[0]);
    }

    if (type === 'Create') {
      updateFormField('primary.masterHttpPort', DEFAULT_PORTS.MASTER_HTTP_PORT);
      updateFormField('primary.masterRpcPort', DEFAULT_PORTS.MASTER_RPC_PORT);
      updateFormField('primary.tserverHttpPort', DEFAULT_PORTS.TSERVER_HTTP_PORT);
      updateFormField('primary.tserverRpcPort', DEFAULT_PORTS.TSERVER_RPC_PORT);
      updateFormField('primary.redisHttpPort', DEFAULT_PORTS.YEDIS_HTTP_PORT);
      updateFormField('primary.redisRpcPort', DEFAULT_PORTS.YEDIS_RPC_PORT);
      updateFormField('primary.yqlHttpPort', DEFAULT_PORTS.YQL_HTTP_PORT);
      updateFormField('primary.yqlRpcPort', DEFAULT_PORTS.YQL_RPC_PORT);
      updateFormField('primary.ysqlHttpPort', DEFAULT_PORTS.YSQL_HTTP_PORT);
      updateFormField('primary.ysqlRpcPort', DEFAULT_PORTS.YSQL_RPC_PORT);
    } else if (type === 'Edit') {
      const { communicationPorts } = universeDetails;
      const customPorts = this.portsCustomized(communicationPorts);
      updateFormField('primary.customizePorts', customPorts);
      this.setState({ customizePorts: customPorts });
      updateFormField('primary.masterHttpPort', communicationPorts.masterHttpPort);
      updateFormField('primary.masterRpcPort', communicationPorts.masterRpcPort);
      updateFormField('primary.tserverHttpPort', communicationPorts.tserverHttpPort);
      updateFormField('primary.tserverRpcPort', communicationPorts.tserverRpcPort);
      updateFormField('primary.redisHttpPort', communicationPorts.redisServerHttpPort);
      updateFormField('primary.redisRpcPort', communicationPorts.redisServerRpcPort);
      updateFormField('primary.yqlHttpPort', communicationPorts.yqlServerHttpPort);
      updateFormField('primary.yqlRpcPort', communicationPorts.yqlServerRpcPort);
      updateFormField('primary.ysqlHttpPort', communicationPorts.ysqlServerHttpPort);
      updateFormField('primary.ysqlRpcPort', communicationPorts.ysqlServerRpcPort);
    }

    if (isNonEmptyObject(formValues['primary']) && clusterType !== 'primary') {
      this.setState({ universeName: formValues['primary'].universeName });
      updateFormField(`${clusterType}.universeName`, formValues['primary'].universeName);
    }

    // This flag will prevent configure from being fired on component load
    if (formValues && isNonEmptyObject(formValues[clusterType])) {
      this.setState({ nodeSetViaAZList: true });
    }

    const isEditReadOnlyFlow = type === 'Async' && this.state.isReadOnlyExists;
    if (type === 'Edit' || isEditReadOnlyFlow) {
      const primaryCluster = getPrimaryCluster(universeDetails.clusters);
      const readOnlyCluster = getReadOnlyCluster(universeDetails.clusters);
      const userIntent =
        clusterType === 'async'
          ? readOnlyCluster && {
            ...readOnlyCluster.userIntent,
            universeName: primaryCluster.userIntent.universeName
          }
          : primaryCluster && primaryCluster.userIntent;
      const providerUUID = userIntent && userIntent.provider;
      const encryptionAtRestEnabled =
        universeDetails.encryptionAtRestConfig &&
        universeDetails.encryptionAtRestConfig.encryptionAtRestEnabled;

      if (userIntent && providerUUID) {
        const storageType =
          userIntent.deviceInfo === null ? null : userIntent.deviceInfo.storageType;
        this.setState({
          isKubernetesUniverse: isKubernetesUniverse(this.props.universe.currentUniverse.data),
          providerSelected: providerUUID,
          instanceTypeSelected: userIntent.instanceType,
          numNodes: userIntent.numNodes,
          replicationFactor: userIntent.replicationFactor,
          ybSoftwareVersion: userIntent.ybSoftwareVersion,
          assignPublicIP: userIntent.assignPublicIP,
          useTimeSync: userIntent.useTimeSync,
          enableYSQL: userIntent.enableYSQL,
          enableIPV6: userIntent.enableIPV6,
          enableYEDIS: userIntent.enableYEDIS,
          enableNodeToNodeEncrypt: userIntent.enableNodeToNodeEncrypt,
          enableClientToNodeEncrypt: userIntent.enableClientToNodeEncrypt,
          enableEncryptionAtRest: encryptionAtRestEnabled,
          accessKeyCode: userIntent.accessKeyCode,
          deviceInfo: userIntent.deviceInfo,
          storageType: storageType,
          regionList: userIntent.regionList,
          volumeType: storageType === null ? 'SSD' : 'EBS' //TODO(wesley): fixme - establish volumetype/storagetype relationship
        });
      }

      this.props.getRegionListItems(providerUUID);
      this.props.getInstanceTypeListItems(providerUUID);
      if (primaryCluster.userIntent.providerType === 'onprem') {
        this.props.fetchNodeInstanceList(providerUUID);
      }
      // If Edit Case Set Initial Configuration
      this.props.getExistingUniverseConfiguration(_.cloneDeep(universeDetails));
    } else {
      // Repopulate the form fields when switching back to the view
      if (formValues && isNonEmptyObject(formValues[clusterType])) {
        this.setState({
          providerType: formValues[clusterType].providerType,
          providerSelected: formValues[clusterType].provider,
          numNodes: formValues[clusterType].numNodes ? formValues[clusterType].numNodes : 3,
          replicationFactor: formValues[clusterType].replicationFactor
            ? Number(formValues[clusterType].replicationFactor)
            : 3
        });
        if (isNonEmptyString(formValues[clusterType].provider)) {
          this.props.getInstanceTypeListItems(formValues[clusterType].provider);
          this.props.getRegionListItems(formValues[clusterType].provider);
          this.setState({ instanceTypeSelected: formValues[clusterType].instanceType });

          if (formValues[clusterType].assignPublicIP) {
            // We would also default to whatever primary cluster's state for this one.
            this.setState({ assignPublicIP: formValues['primary'].assignPublicIP });
          }
          if (formValues[clusterType].useTimeSync) {
            // We would also default to whatever primary cluster's state for this one.
            this.setState({ useTimeSync: formValues['primary'].useTimeSync });
          }
          if (formValues[clusterType].enableYSQL) {
            // We would also default to whatever primary cluster's state for this one.
            this.setState({ enableYSQL: formValues['primary'].enableYSQL });
          }
          if (formValues[clusterType].enableIPV6) {
            // We would also default to whatever primary cluster's state for this one.
            this.setState({ enableIPV6: formValues['primary'].enableIPV6 });
          }
          if (formValues[clusterType].enableYEDIS) {
            // We would also default to whatever primary cluster's state for this one.
            this.setState({ enableYEDIS: formValues['primary'].enableYEDIS });
          }
          if (formValues[clusterType].enableNodeToNodeEncrypt) {
            // We would also default to whatever primary cluster's state for this one.
            this.setState({
              enableNodeToNodeEncrypt: formValues['primary'].enableNodeToNodeEncrypt
            });
          }
          if (formValues[clusterType].enableClientToNodeEncrypt) {
            // We would also default to whatever primary cluster's state for this one.
            this.setState({
              enableClientToNodeEncrypt: formValues['primary'].enableClientToNodeEncrypt
            });
          }
        }
      } else {
        // Initialize the form values if not exists
        updateFormField(`${clusterType}.numNodes`, 3);
        updateFormField(`${clusterType}.replicationFactor`, 3);
      }
    }
  }

  UNSAFE_componentWillReceiveProps(nextProps) {
    const {
      universe: { currentUniverse },
      cloud: { nodeInstanceList, instanceTypes },
      clusterType,
      formValues
    } = nextProps;

    const currentFormValues = formValues[clusterType];
    let providerSelected = this.state.providerSelected;
    if (isNonEmptyObject(currentFormValues) && isNonEmptyString(currentFormValues.provider)) {
      providerSelected = currentFormValues.provider;
    }

    if (
      nextProps.cloud.instanceTypes.data !== this.props.cloud.instanceTypes.data &&
      isNonEmptyArray(nextProps.cloud.instanceTypes.data) &&
      providerSelected
    ) {
      if (
        nextProps.type === 'Create' ||
        (nextProps.type === 'Async' && !this.state.isReadOnlyExists)
      ) {
        let instanceTypeSelected = null;
        const currentProviderCode = this.getCurrentProvider(providerSelected).code;
        instanceTypeSelected = DEFAULT_INSTANCE_TYPE_MAP[currentProviderCode];
        // If we have the default instance type in the cloud instance types then we
        // use it, otherwise we pick the first one in the list and use it.
        const hasInstanceType = instanceTypes.data.find((it) => {
          return (
            it.providerCode === currentProviderCode && it.instanceTypeCode === instanceTypeSelected
          );
        });
        if (!hasInstanceType) {
          instanceTypeSelected = instanceTypes.data[0].instanceTypeCode;
        }

        const instanceTypeSelectedData = instanceTypes.data.find(function (item) {
          return item.instanceTypeCode === formValues[clusterType].instanceType;
        });

        if (isNonEmptyObject(instanceTypeSelectedData)) {
          instanceTypeSelected = formValues[clusterType].instanceType;
        }

        this.props.updateFormField(`${clusterType}.instanceType`, instanceTypeSelected);
        this.setState({ instanceTypeSelected: instanceTypeSelected });
        this.setDeviceInfo(instanceTypeSelected, instanceTypes.data);
      }
    }

    const currentProvider = this.getCurrentProvider(providerSelected);
    // Set default storageType once API call has completed, defaults to AWS provider if current provider is not GCP
    if (
      typeof currentProvider !== 'undefined' &&
      currentProvider.code === 'gcp' &&
      isNonEmptyArray(nextProps.cloud.gcpTypes.data) &&
      !isNonEmptyArray(this.props.cloud.gcpTypes.data)
    ) {
      this.props.updateFormField(`${clusterType}.storageType`, DEFAULT_STORAGE_TYPES['GCP']);
      this.setState({ storageType: DEFAULT_STORAGE_TYPES['GCP'] });
    } else if (
      isNonEmptyArray(nextProps.cloud.ebsTypes) &&
      !isNonEmptyArray(this.props.cloud.ebsTypes)
    ) {
      this.props.updateFormField(`${clusterType}.storageType`, DEFAULT_STORAGE_TYPES['AWS']);
      this.setState({ storageType: DEFAULT_STORAGE_TYPES['AWS'] });
    }

    if (
      isNonEmptyArray(nextProps.softwareVersions) &&
      isNonEmptyObject(this.props.formValues[clusterType]) &&
      !isNonEmptyString(this.props.formValues[clusterType].ybSoftwareVersion)
    ) {
      this.setState({ ybSoftwareVersion: this.props.softwareVersions[0] });
      this.props.updateFormField(`${clusterType}.ybSoftwareVersion`, nextProps.softwareVersions[0]);
    }

    // Form Actions on Create Universe Success
    if (
      getPromiseState(this.props.universe.createUniverse).isLoading() &&
      getPromiseState(nextProps.universe.createUniverse).isSuccess()
    ) {
      this.props.reset();
      this.props.fetchUniverseMetadata();
      this.props.fetchCustomerTasks();
      if (this.context.prevPath) {
        browserHistory.push(this.context.prevPath);
      } else {
        browserHistory.push('/universes');
      }
    }
    // Form Actions on Edit Universe Success
    if (
      (getPromiseState(this.props.universe.editUniverse).isLoading() &&
        getPromiseState(nextProps.universe.editUniverse).isSuccess()) ||
      (getPromiseState(this.props.universe.addReadReplica).isLoading() &&
        getPromiseState(nextProps.universe.addReadReplica).isSuccess()) ||
      (getPromiseState(this.props.universe.editReadReplica).isLoading() &&
        getPromiseState(nextProps.universe.editReadReplica).isSuccess()) ||
      (getPromiseState(this.props.universe.deleteReadReplica).isLoading() &&
        getPromiseState(nextProps.universe.deleteReadReplica).isSuccess())
    ) {
      this.props.fetchCurrentUniverse(currentUniverse.data.universeUUID);
      this.props.fetchUniverseMetadata();
      this.props.fetchCustomerTasks();
      this.props.fetchUniverseTasks(currentUniverse.data.universeUUID);
      browserHistory.push(this.props.location.pathname);
    }
    // Form Actions on Configure Universe Success
    if (
      getPromiseState(this.props.universe.universeConfigTemplate).isLoading() &&
      getPromiseState(nextProps.universe.universeConfigTemplate).isSuccess()
    ) {
      this.props.fetchUniverseResources(nextProps.universe.universeConfigTemplate.data);
    }
    // If nodeInstanceList changes, fetch number of available nodes
    if (
      getPromiseState(nodeInstanceList).isSuccess() &&
      getPromiseState(this.props.cloud.nodeInstanceList).isLoading()
    ) {
      let numNodesAvailable = nodeInstanceList.data.reduce((acc, val) => {
        if (!val.inUse) {
          acc++;
        }
        return acc;
      }, 0);
      // Add Existing nodes in Universe userIntent to available nodes for calculation in case of Edit
      if (
        this.props.type === 'Edit' ||
        (nextProps.type === 'Async' && !this.state.isReadOnlyExists)
      ) {
        const cluster = getClusterByType(
          currentUniverse.data.universeDetails.clusters,
          clusterType
        );
        if (isDefinedNotNull(cluster)) {
          numNodesAvailable += cluster.userIntent.numNodes;
        }
      }
      this.setState({ maxNumNodes: numNodesAvailable });
    }
  }
  componentDidUpdate(prevProps, prevState) {
    const {
      universe: { currentUniverse, universeConfigTemplate },
      formValues,
      clusterType,
      setPlacementStatus,
      toggleDisableSubmit,
      type
    } = this.props;
    let currentProviderUUID = this.state.providerSelected;

    if (
      isNonEmptyObject(formValues[clusterType]) &&
      isNonEmptyString(formValues[clusterType].provider)
    ) {
      currentProviderUUID = formValues[clusterType].provider;
    }
    const currentProvider = this.getCurrentProvider(currentProviderUUID);

    const configureIntentValid = () => {
      return (
        !_.isEqual(this.state, prevState) &&
        isNonEmptyObject(currentProvider) &&
        isNonEmptyArray(formValues[clusterType].regionList) &&
        (prevState.maxNumNodes !== -1 || currentProvider.code !== 'onprem') &&
        ((currentProvider.code === 'onprem' && this.state.numNodes <= this.state.maxNumNodes) ||
          currentProvider.code !== 'onprem') &&
        this.state.numNodes >= this.state.replicationFactor &&
        !this.state.nodeSetViaAZList
      );
    };

    // Fire Configure only if either provider is not on-prem or maxNumNodes is not -1 if on-prem
    if (configureIntentValid()) {
      toggleDisableSubmit(false);
      if (isNonEmptyObject(currentUniverse.data)) {
        if (!this.hasFieldChanged()) {
          const placementStatusObject = {
            error: {
              type: 'noFieldsChanged',
              numNodes: this.state.numNodes,
              maxNumNodes: this.state.maxNumNodes
            }
          };
          setPlacementStatus(placementStatusObject);
        }
      }
      this.configureUniverseNodeList();
    } else if (currentProvider && currentProvider.code === 'onprem') {
      toggleDisableSubmit(false);
      if (
        isNonEmptyArray(this.state.regionList) &&
        currentProvider &&
        this.state.instanceTypeSelected &&
        this.state.numNodes > this.state.maxNumNodes
      ) {
        const placementStatusObject = {
          error: {
            type: 'notEnoughNodesConfigured',
            numNodes: this.state.numNodes,
            maxNumNodes: this.state.maxNumNodes
          }
        };
        setPlacementStatus(placementStatusObject);
        toggleDisableSubmit(true);
      } else if (isNonEmptyObject(currentUniverse.data)) {
        const primaryCluster = currentUniverse.data.universeDetails.clusters.find(
          (x) => x.clusterType === 'PRIMARY'
        );
        const provider = primaryCluster.placementInfo.cloudList.find(
          (c) => c.uuid === currentProvider.uuid
        );
        const replication = primaryCluster.userIntent.replicationFactor;
        if (provider) {
          const numAzs = provider.regionList.reduce(
            (acc, current) => acc + current.azList.length,
            0
          );
          setPlacementStatus({
            replicationFactor: replication,
            numUniqueAzs: numAzs,
            numUniqueRegions: provider.regionList.length
          });
        }
      }
    }
    //hook from parent universeForm to check if any fields was changed
    const nodeDetailsSet =
      getPromiseState(currentUniverse).isSuccess() &&
      getPromiseState(universeConfigTemplate).isSuccess()
        ? universeConfigTemplate.data.nodeDetailsSet
        : [];
    if (type === 'Edit' || (this.props.type === 'Async' && this.state.isReadOnlyExists)) {
      this.props.handleHasFieldChanged(
        this.hasFieldChanged() ||
        !_.isEqual(currentUniverse.data.universeDetails.nodeDetailsSet, nodeDetailsSet)
      );
    } else {
      this.props.handleHasFieldChanged(true);
    }
  }

  numNodesChangedViaAzList(value) {
    const { updateFormField, clusterType } = this.props;
    this.setState({ nodeSetViaAZList: true, numNodes: value });
    updateFormField(`${clusterType}.numNodes`, value);
  }

  setDeviceInfo(instanceTypeCode, instanceTypeList) {
    const { updateFormField, clusterType } = this.props;
    const instanceTypeSelectedData = instanceTypeList.find(function (item) {
      return item.instanceTypeCode === instanceTypeCode;
    });
    const volumesList = instanceTypeSelectedData.instanceTypeDetails.volumeDetailsList;
    const volumeDetail = volumesList[0];
    let mountPoints = null;
    if (instanceTypeSelectedData.providerCode === 'onprem') {
      mountPoints = instanceTypeSelectedData.instanceTypeDetails.volumeDetailsList
        .map(function (item) {
          return item.mountPath;
        })
        .join(',');
    }
    if (volumeDetail) {
      const deviceInfo = {
        volumeSize: volumeDetail.volumeSizeGB,
        numVolumes: volumesList.length,
        mountPoints: mountPoints,
        storageType:
          volumeDetail.volumeType === 'EBS'
            ? DEFAULT_STORAGE_TYPES['AWS']
            : DEFAULT_STORAGE_TYPES['GCP'],
        storageClass: 'standard',
        diskIops: null
      };
      updateFormField(`${clusterType}.volumeSize`, volumeDetail.volumeSizeGB);
      updateFormField(`${clusterType}.numVolumes`, volumesList.length);
      updateFormField(`${clusterType}.diskIops`, volumeDetail.diskIops);
      updateFormField(`${clusterType}.storageType`, volumeDetail.storageType);
      updateFormField(`${clusterType}.mountPoints`, mountPoints);
      this.setState({ deviceInfo: deviceInfo, volumeType: volumeDetail.volumeType });
    }
  }

  softwareVersionChanged(value) {
    const { updateFormField, clusterType } = this.props;
    this.setState({ ybSoftwareVersion: value });
    updateFormField(`${clusterType}.ybSoftwareVersion`, value);
  }

  storageTypeChanged(storageValue) {
    const { updateFormField, clusterType } = this.props;
    const currentDeviceInfo = _.clone(this.state.deviceInfo);
    currentDeviceInfo.storageType = storageValue;
    if (currentDeviceInfo.storageType === 'IO1' && currentDeviceInfo.diskIops == null) {
      currentDeviceInfo.diskIops = 1000;
      updateFormField(`${clusterType}.diskIops`, 1000);
    } else {
      currentDeviceInfo.diskIops = null;
    }
    updateFormField(`${clusterType}.storageType`, storageValue);
    this.setState({ deviceInfo: currentDeviceInfo, storageType: storageValue });
  }

  numVolumesChanged(val) {
    const { updateFormField, clusterType } = this.props;
    updateFormField(`${clusterType}.numVolumes`, val);
    this.setState({ deviceInfo: { ...this.state.deviceInfo, numVolumes: val } });
  }

  volumeSizeChanged(val) {
    const { updateFormField, clusterType } = this.props;
    updateFormField(`${clusterType}.volumeSize`, val);
    this.setState({ deviceInfo: { ...this.state.deviceInfo, volumeSize: val } });
  }

  diskIopsChanged(val) {
    const { updateFormField, clusterType } = this.props;
    updateFormField(`${clusterType}.diskIops`, val);
    if (this.state.deviceInfo.storageType === 'IO1') {
      this.setState({ deviceInfo: { ...this.state.deviceInfo, diskIops: val } });
    }
  }

  toggleUseTimeSync(event) {
    const { updateFormField, clusterType } = this.props;
    updateFormField(`${clusterType}.useTimeSync`, event.target.checked);
    this.setState({ useTimeSync: event.target.checked });
  }

  toggleAssignPublicIP(event) {
    const { updateFormField, clusterType } = this.props;
    // Right now we only let primary cluster to update this flag, and
    // keep the async cluster to use the same value as primary.
    if (clusterType === 'primary') {
      updateFormField('primary.assignPublicIP', event.target.checked);
      updateFormField('async.assignPublicIP', event.target.checked);
      this.setState({ assignPublicIP: event.target.checked });
    }
  }

  toggleEnableYSQL(event) {
    const { updateFormField, clusterType } = this.props;
    // Right now we only let primary cluster to update this flag, and
    // keep the async cluster to use the same value as primary.
    if (clusterType === 'primary') {
      updateFormField('primary.enableYSQL', event.target.checked);
      updateFormField('async.enableYSQL', event.target.checked);
      this.setState({ enableYSQL: event.target.checked });
    }
  }

  toggleEnableIPV6(event) {
    const { updateFormField, clusterType } = this.props;
    // Right now we only let primary cluster to update this flag, and
    // keep the async cluster to use the same value as primary.
    if (clusterType === 'primary') {
      updateFormField('primary.enableIPV6', event.target.checked);
      updateFormField('async.enableIPV6', event.target.checked);
      this.setState({ enableIPV6: event.target.checked });
    }
  }

  toggleEnableYEDIS(event) {
    const { updateFormField, clusterType } = this.props;
    // Right now we only let primary cluster to update this flag, and
    // keep the async cluster to use the same value as primary.
    if (clusterType === 'primary') {
      updateFormField('primary.enableYEDIS', event.target.checked);
      updateFormField('async.enableYEDIS', event.target.checked);
      this.setState({ enableYEDIS: event.target.checked });
    }
  }

  toggleEnableNodeToNodeEncrypt(event) {
    const { updateFormField, clusterType } = this.props;
    // Right now we only let primary cluster to update this flag, and
    // keep the async cluster to use the same value as primary.
    if (clusterType === 'primary') {
      updateFormField('primary.enableNodeToNodeEncrypt', event.target.checked);
      updateFormField('async.NodeToNodeEncrypt', event.target.checked);
      this.setState({
        enableNodeToNodeEncrypt: event.target.checked,
        enableClientToNodeEncrypt: this.state.enableClientToNodeEncrypt && event.target.checked});
    }
  }

  toggleEnableClientToNodeEncrypt(event) {
    const { updateFormField, clusterType } = this.props;
    // Right now we only let primary cluster to update this flag, and
    // keep the async cluster to use the same value as primary.
    if (clusterType === 'primary') {
      updateFormField('primary.enableClientToNodeEncrypt', event.target.checked);
      updateFormField('async.ClientToNodeEncrypt', event.target.checked);
      this.setState({ enableClientToNodeEncrypt: event.target.checked });
    }
  }

  toggleEnableEncryptionAtRest(event) {
    const { updateFormField, clusterType } = this.props;
    if (clusterType === 'primary') {
      updateFormField('primary.enableEncryptionAtRest', event.target.checked);
      this.setState({ enableEncryptionAtRest: event.target.checked });
    }
  }

  toggleCustomizePorts(event) {
    this.setState({ customizePorts: event.target.checked });
  }

  handleAwsArnChange(event) {
    const { updateFormField } = this.props;
    updateFormField('primary.awsArnString', event.target.value);
  }

  handleSelectAuthConfig(value) {
    const { updateFormField, clusterType } = this.props;
    updateFormField(`${clusterType}.selectEncryptionAtRestConfig`, value);
    this.setState({ selectEncryptionAtRestConfig: value });
  }

  replicationFactorChanged = (value) => {
    const {
      updateFormField,
      clusterType,
      universe: {
        currentUniverse: { data }
      }
    } = this.props;
    const clusterExists = isDefinedNotNull(data.universeDetails)
      ? isEmptyObject(getClusterByType(data.universeDetails.clusters, clusterType))
      : null;
    const self = this;

    if (!clusterExists) {
      this.setState({ nodeSetViaAZList: false, replicationFactor: value }, function () {
        if (self.state.numNodes <= value) {
          self.setState({ numNodes: value });
          updateFormField(`${clusterType}.numNodes`, value);
        }
      });
    }
    updateFormField(`${clusterType}.replicationFactor`, value);
  };

  hasFieldChanged = () => {
    const {
      universe: { currentUniverse },
      clusterType,
      getCurrentUserIntent
    } = this.props;
    if (
      isEmptyObject(currentUniverse.data) ||
      isEmptyObject(currentUniverse.data.universeDetails)
    ) {
      return true;
    }
    const currentCluster = getClusterByType(
      currentUniverse.data.universeDetails.clusters,
      clusterType
    );
    const existingIntent = isNonEmptyObject(currentCluster)
      ? _.clone(currentCluster.userIntent, true)
      : null;
    const currentIntent = getCurrentUserIntent(clusterType);

    return !areIntentsEqual(existingIntent, currentIntent);
  };

  handleUniverseConfigure(universeTaskParams) {
    const {
      universe: { universeConfigTemplate, currentUniverse },
      formValues,
      clusterType
    } = this.props;

    const instanceType = formValues[clusterType].instanceType;
    const regionList = formValues[clusterType].regionList;
    const verifyIntentConditions = function () {
      return isNonEmptyArray(regionList) && isNonEmptyString(instanceType);
    };

    if (verifyIntentConditions()) {
      if (
        isNonEmptyObject(currentUniverse.data) &&
        isNonEmptyObject(currentUniverse.data.universeDetails) &&
        isDefinedNotNull(
          getClusterByType(currentUniverse.data.universeDetails.clusters, clusterType)
        )
      ) {
        // cluster set: main edit flow
        const oldCluster = getClusterByType(
          currentUniverse.data.universeDetails.clusters,
          clusterType
        );
        const newCluster = getClusterByType(universeTaskParams.clusters, clusterType);
        if (
          isNonEmptyObject(oldCluster) &&
          isNonEmptyObject(newCluster) &&
          areIntentsEqual(oldCluster.userIntent, newCluster.userIntent)
        ) {
          this.props.getExistingUniverseConfiguration(currentUniverse.data.universeDetails);
        } else {
          this.props.submitConfigureUniverse(universeTaskParams);
        }
      } else {
        // Create flow
        if (isEmptyObject(universeConfigTemplate.data) || universeConfigTemplate.data == null) {
          this.props.submitConfigureUniverse(universeTaskParams);
        } else {
          const currentClusterConfiguration = getClusterByType(
            universeConfigTemplate.data.clusters,
            clusterType
          );
          if (!isDefinedNotNull(currentClusterConfiguration)) {
            this.props.submitConfigureUniverse(universeTaskParams);
          } else if (
            !areIntentsEqual(
              getClusterByType(universeTaskParams.clusters, clusterType).userIntent,
              currentClusterConfiguration.userIntent
            )
          ) {
            this.props.submitConfigureUniverse(universeTaskParams);
          }
        }
      }
    }
  }

  configureUniverseNodeList() {
    const {
      universe: { universeConfigTemplate, currentUniverse },
      formValues,
      clusterType,
      getCurrentUserIntent,
      updateTaskParams
    } = this.props;
    const { hasInstanceTypeChanged } = this.state;
    const currentProviderUUID = this.state.providerSelected;
    let universeTaskParams = {};
    if (isNonEmptyObject(universeConfigTemplate.data)) {
      universeTaskParams = _.cloneDeep(universeConfigTemplate.data);
    }
    if (
      this.props.type === 'Async' &&
      !isDefinedNotNull(getReadOnlyCluster(currentUniverse.data.universeDetails.clusters))
    ) {
      universeTaskParams = _.cloneDeep(currentUniverse.data.universeDetails);
    }
    if (isNonEmptyObject(currentUniverse.data)) {
      universeTaskParams.universeUUID = currentUniverse.data.universeUUID;
      universeTaskParams.expectedUniverseVersion = currentUniverse.data.version;
    }

    const userIntent = getCurrentUserIntent(clusterType);
    if (
      hasInstanceTypeChanged !==
      (formValues[clusterType].instanceType !== this.currentInstanceType)
    ) {
      this.setState({ hasInstanceTypeChanged: !hasInstanceTypeChanged });
    }

    if (
      isNonEmptyObject(formValues[clusterType].instanceTags) &&
      currentProviderUUID &&
      this.getCurrentProvider(currentProviderUUID).code === 'aws'
    ) {
      userIntent['instanceTags'] = formValues[clusterType].instanceTags;
    }

    this.props.cloud.providers.data.forEach(function (providerItem) {
      if (providerItem.uuid === formValues[clusterType].provider) {
        userIntent.providerType = providerItem.code;
      }
    });

    const isEdit =
      this.props.type === 'Edit' || (this.props.type === 'Async' && this.state.isReadOnlyExists);
    updateTaskParams(universeTaskParams, userIntent, clusterType, isEdit);
    universeTaskParams.userAZSelected = false;
    this.handleUniverseConfigure(universeTaskParams);
  }

  numNodesChanged(value) {
    const { updateFormField, clusterType } = this.props;
    this.setState({ numNodes: value, nodeSetViaAZList: false });
    updateFormField(`${clusterType}.numNodes`, value);
  }

  getCurrentProvider(providerUUID) {
    return this.props.cloud.providers.data.find((provider) => provider.uuid === providerUUID);
  }

  providerChanged = (value) => {
    const {
      updateFormField,
      clusterType,
      universe: {
        currentUniverse: { data }
      }
    } = this.props;
    const providerUUID = value;
    const currentProviderData = this.getCurrentProvider(value) || {};

    const targetCluster =
      clusterType !== 'primary'
        ? isNonEmptyObject(data) && getPrimaryCluster(data.universeDetails.clusters)
        : isNonEmptyObject(data) && getReadOnlyCluster(data.universeDetails.clusters);
    if (isEmptyObject(data) || isDefinedNotNull(targetCluster)) {
      this.props.updateFormField(`${clusterType}.regionList`, []);
      //If we have accesskeys for a current selected provider we set that in the state or we fallback to default value.
      let defaultAccessKeyCode = initialState.accessKeyCode;
      if (isNonEmptyArray(this.props.accessKeys.data)) {
        const providerAccessKeys = this.props.accessKeys.data.filter(
          (key) => key.idKey.providerUUID === value
        );
        if (isNonEmptyArray(providerAccessKeys)) {
          defaultAccessKeyCode = providerAccessKeys[0].idKey.keyCode;
        }
      }
      updateFormField(`${clusterType}.accessKeyCode`, defaultAccessKeyCode);

      if (currentProviderData.code === 'gcp') {
        this.storageTypeChanged(DEFAULT_STORAGE_TYPES['GCP']);
      } else if (currentProviderData.code === 'aws') {
        this.storageTypeChanged(DEFAULT_STORAGE_TYPES['AWS']);
      } else if (currentProviderData.code === 'azu') {
        this.storageTypeChanged(DEFAULT_STORAGE_TYPES['AZU']);
      }

      this.setState({
        nodeSetViaAZList: false,
        regionList: [],
        providerSelected: providerUUID,
        deviceInfo: {},
        accessKeyCode: defaultAccessKeyCode
      });
      this.props.getRegionListItems(providerUUID, true);
      this.props.getInstanceTypeListItems(providerUUID);
    }

    if (currentProviderData.code === 'onprem') {
      this.props.fetchNodeInstanceList(value);
    }
    this.setState({
      isKubernetesUniverse: currentProviderData.code === 'kubernetes'
    });
  };

  accessKeyChanged(event) {
    const { clusterType } = this.props;
    this.props.updateFormField(`${clusterType}.accessKeyCode`, event.target.value);
  }

  instanceTypeChanged(value) {
    const { updateFormField, clusterType } = this.props;
    const instanceTypeValue = value;
    updateFormField(`${clusterType}.instanceType`, instanceTypeValue);
    this.setState({ instanceTypeSelected: instanceTypeValue, nodeSetViaAZList: false });

    this.setDeviceInfo(instanceTypeValue, this.props.cloud.instanceTypes.data);
  }

  regionListChanged(value) {
    const {
      formValues,
      clusterType,
      updateFormField,
      cloud: { providers }
    } = this.props;
    this.setState({ nodeSetViaAZList: false, regionList: value });
    const currentProvider = providers.data.find((a) => a.uuid === formValues[clusterType].provider);
    if (!isNonEmptyString(formValues[clusterType].instanceType)) {
      updateFormField(
        `${clusterType}.instanceType`,
        DEFAULT_INSTANCE_TYPE_MAP[currentProvider.code]
      );
    }
  }

  /**
   * This method is used to disable the ClientToNodeTLS field initially.
   * Once the NodeToNode TLS is enabled, then ClientToNode TLS will be editable.
   * If ClientToNode TLS sets to enable and NodeToNode TLS sets to disable then
   * ClientToNode TLS will be disabled.
   * 
   * @param isFieldReadOnly If true then readonly access.
   * @param enableNodeToNodeEncrypt NodeToNodeTLS state.
   */
  clientToNodeEncryptField(isFieldReadOnly, enableNodeToNodeEncrypt) {
    return isFieldReadOnly || !enableNodeToNodeEncrypt;
  }

  render() {
    const { clusterType, cloud, softwareVersions, accessKeys, universe, formValues } = this.props;
    const { hasInstanceTypeChanged } = this.state;
    const self = this;
    let gflagArray = <span />;
    let tagsArray = <span />;
    let universeProviderList = [];
    let currentProviderCode = '';

    let currentProviderUUID = self.state.providerSelected;
    if (formValues[clusterType] && formValues[clusterType].provider) {
      currentProviderUUID = formValues[clusterType].provider;
    }

    // Populate the cloud provider list
    if (isNonEmptyArray(cloud.providers.data)) {
      universeProviderList = cloud.providers.data.map(function (providerItem, idx) {
        if (providerItem.uuid === currentProviderUUID) {
          currentProviderCode = providerItem.code;
        }
        return (
          <option key={providerItem.uuid} value={providerItem.uuid}>
            {providerItem.name}
          </option>
        );
      });
    }

    // Spot price and EBS types
    let storageTypeSelector = <span />;
    let deviceDetail = null;
    let iopsField = <span />;
    function volumeTypeFormat(num) {
      return num + ' GB';
    }
    const ebsTypesList =
      cloud.ebsTypes &&
      cloud.ebsTypes.sort().map(function (ebsType, idx) {
        return (
          <option key={ebsType} value={ebsType}>
            {ebsType}
          </option>
        );
      });
    const gcpTypesList =
      cloud.gcpTypes.data &&
      cloud.gcpTypes.data.sort().map(function (gcpType, idx) {
        return (
          <option key={gcpType} value={gcpType}>
            {API_UI_STORAGE_TYPES[gcpType]}
          </option>
        );
      });
    const azuTypesList =
      cloud.azuTypes.data &&
      cloud.azuTypes.data.sort().map(function (azuType, idx) {
        return (
          <option key={azuType} value={azuType}>
            {API_UI_STORAGE_TYPES[azuType]}
          </option>
        );
      });

    const kmsConfigList = [
      <option value="0" key={`kms-option-0`}>
        Select Configuration
      </option>,
      ...cloud.authConfig.data.map((config, index) => {
        const labelName = config.metadata.provider + ' - ' + config.metadata.name;
        return (
          <option value={config.metadata.configUUID} key={`kms-option-${index + 1}`}>
            {labelName}
          </option>
        );
      })
    ];
    const isFieldReadOnly =
      isNonEmptyObject(universe.currentUniverse.data) &&
      (this.props.type === 'Edit' || (this.props.type === 'Async' && this.state.isReadOnlyExists));

    //Get list of cloud providers
    const providerNameField = (
      <Field
        name={`${clusterType}.provider`}
        type="select"
        component={YBSelectWithLabel}
        label="Provider"
        onInputChanged={this.providerChanged}
        options={universeProviderList}
        readOnlySelect={isFieldReadOnly}
      />
    );

    const deviceInfo = this.state.deviceInfo;

    if (isNonEmptyObject(formValues[clusterType])) {
      const currentCluster = formValues[clusterType];
      if (isNonEmptyString(currentCluster.numVolumes)) {
        deviceInfo['numVolumes'] = currentCluster.numVolumes;
      }
      if (isNonEmptyString(currentCluster.volumeSize)) {
        deviceInfo['volumeSize'] = currentCluster.volumeSize;
      }
      if (isNonEmptyString(currentCluster.diskIops)) {
        deviceInfo['diskIops'] = currentCluster.diskIops;
      }
      if (isNonEmptyObject(currentCluster.storageType)) {
        deviceInfo['storageType'] = currentCluster.storageType;
      }
    }

    if (isNonEmptyObject(deviceInfo)) {
      const currentProvider = this.getCurrentProvider(self.state.providerSelected);
      if (
        (self.state.volumeType === 'EBS' || self.state.volumeType === 'SSD'
          || self.state.volumeType === 'NVME') &&
        isDefinedNotNull(currentProvider)
      ) {
        const isInAws = currentProvider.code === 'aws';
        const isInGcp = currentProvider.code === 'gcp';
        const isInAzu = currentProvider.code === 'azu';
        // We don't want to keep the volume fixed in case of Kubernetes or persistent GCP storage.
        const fixedVolumeInfo =
          (self.state.volumeType === 'SSD' || self.state.volumeType === 'NVME') &&
          currentProvider.code !== 'kubernetes' &&
          deviceInfo.storageType === 'Scratch' &&
          currentProvider.code !== 'azu';
        const fixedNumVolumes =
          (self.state.volumeType === 'SSD' || self.state.volumeType === 'NVME') &&
          currentProvider.code !== 'kubernetes' &&
          currentProvider.code !== 'gcp' &&
          currentProvider.code !== 'azu';
        const isIoType = deviceInfo.storageType === 'IO1';
        if (isIoType) {
          iopsField = (
            <span className="volume-info form-group-shrinked  volume-info-iops">
              <label className="form-item-label">Provisioned IOPS</label>
              <span className="volume-info-field">
                <Field
                  name={`${clusterType}.diskIops`}
                  component={YBUnControlledNumericInput}
                  label="Provisioned IOPS"
                  onInputChanged={self.diskIopsChanged}
                  readOnly={isFieldReadOnly}
                />
              </span>
            </span>
          );
        }
        const numVolumes = (
          <span className="volume-info-field volume-info-count">
            <Field
              name={`${clusterType}.numVolumes`}
              component={YBUnControlledNumericInput}
              label="Number of Volumes"
              onInputChanged={self.numVolumesChanged}
              readOnly={fixedNumVolumes || !hasInstanceTypeChanged}
            />
          </span>
        );
        const volumeSize = (
          <span className="volume-info-field volume-info-size">
            <Field
              name={`${clusterType}.volumeSize`}
              component={YBUnControlledNumericInput}
              label="Volume Size"
              valueFormat={volumeTypeFormat}
              onInputChanged={self.volumeSizeChanged}
              readOnly={fixedVolumeInfo || !hasInstanceTypeChanged}
            />
          </span>
        );
        deviceDetail = (
          <span className="volume-info">
            {numVolumes}
            &times;
            {volumeSize}
          </span>
        );
        // Only for AWS EBS or GCP, show type option.
        if (isInAws && self.state.volumeType === 'EBS') {
          storageTypeSelector = (
            <span className="volume-info form-group-shrinked">
              <Field
                name={`${clusterType}.storageType`}
                component={YBSelectWithLabel}
                options={ebsTypesList}
                label="EBS Type"
                defaultValue={DEFAULT_STORAGE_TYPES['AWS']}
                onInputChanged={self.storageTypeChanged}
                readOnlySelect={isFieldReadOnly}
              />
            </span>
          );
        } else if (isInGcp) {
          storageTypeSelector = (
            <span className="volume-info form-group-shrinked">
              <Field
                name={`${clusterType}.storageType`}
                component={YBSelectWithLabel}
                options={gcpTypesList}
                label="Storage Type (SSD)"
                defaultValue={DEFAULT_STORAGE_TYPES['GCP']}
                onInputChanged={self.storageTypeChanged}
                readOnlySelect={isFieldReadOnly}
              />
            </span>
          );
        } else if (isInAzu) {
          storageTypeSelector = (
            <span className="volume-info form-group-shrinked">
              <Field
                name={`${clusterType}.storageType`}
                component={YBSelectWithLabel}
                options={azuTypesList}
                label="Storage Type (SSD)"
                defaultValue={DEFAULT_STORAGE_TYPES['AZU']}
                onInputChanged={self.storageTypeChanged}
                readOnlySelect={isFieldReadOnly}
              />
            </span>
          );
        }
      }
    }

    let assignPublicIP = <span />;
    let useTimeSync = <span />;
    let enableYSQL = <span />;
    let enableYEDIS = <span />;
    let enableNodeToNodeEncrypt = <span />;
    let enableClientToNodeEncrypt = <span />;
    let selectTlsCert = <span />;
    let enableEncryptionAtRest = <span />;
    let selectEncryptionAtRestConfig = <span />;
    const currentProvider = this.getCurrentProvider(currentProviderUUID);
    const disableToggleOnChange = clusterType !== 'primary';
    if (
      isDefinedNotNull(currentProvider) &&
      (currentProvider.code === 'aws' ||
        currentProvider.code === 'gcp' ||
        currentProvider.code === 'azu' ||
        currentProvider.code === 'onprem' ||
        currentProvider.code === 'kubernetes')
    ) {
      enableYSQL = (
        <Field
          name={`${clusterType}.enableYSQL`}
          component={YBToggle}
          isReadOnly={isFieldReadOnly}
          disableOnChange={disableToggleOnChange}
          checkedVal={this.state.enableYSQL}
          onToggle={this.toggleEnableYSQL}
          label="Enable YSQL"
          subLabel="Whether or not to enable YSQL."
        />
      );
      enableYEDIS = (
        <Field
          name={`${clusterType}.enableYEDIS`}
          component={YBToggle}
          isReadOnly={isFieldReadOnly}
          disableOnChange={disableToggleOnChange}
          checkedVal={this.state.enableYEDIS}
          onToggle={this.toggleEnableYEDIS}
          label="Enable YEDIS"
          subLabel="Whether or not to enable YEDIS."
        />
      );
      enableNodeToNodeEncrypt = (
        <Field
          name={`${clusterType}.enableNodeToNodeEncrypt`}
          component={YBToggle}
          isReadOnly={isFieldReadOnly}
          disableOnChange={disableToggleOnChange}
          checkedVal={this.state.enableNodeToNodeEncrypt}
          onToggle={this.toggleEnableNodeToNodeEncrypt}
          label="Enable Node-to-Node TLS"
          subLabel="Whether or not to enable TLS Encryption for node to node communication."
        />
      );
      enableClientToNodeEncrypt = (
        <Field
          name={`${clusterType}.enableClientToNodeEncrypt`}
          component={YBToggle}
          isReadOnly={ this.clientToNodeEncryptField(isFieldReadOnly, this.state.enableNodeToNodeEncrypt)}
          disableOnChange={disableToggleOnChange}
          checkedVal={this.state.enableClientToNodeEncrypt}
          onToggle={this.toggleEnableClientToNodeEncrypt}
          label="Enable Client-to-Node TLS"
          subLabel="Whether or not to enable TLS encryption for client to node communication."
        />
      );
      enableEncryptionAtRest = (
        <Field
          name={`${clusterType}.enableEncryptionAtRest`}
          component={YBToggle}
          isReadOnly={isFieldReadOnly}
          disableOnChange={disableToggleOnChange}
          checkedVal={this.state.enableEncryptionAtRest}
          onToggle={this.toggleEnableEncryptionAtRest}
          label="Enable Encryption at Rest"
          title="Upload encryption key file"
          subLabel="Enable encryption for data stored on tablet servers."
        />
      );

      if (this.state.enableEncryptionAtRest) {
        selectEncryptionAtRestConfig = (
          <Field
            name={`${clusterType}.selectEncryptionAtRestConfig`}
            component={YBSelectWithLabel}
            label="Key Management Service Config"
            options={kmsConfigList}
            onInputChanged={this.handleSelectAuthConfig}
            readOnlySelect={isFieldReadOnly}
          />
        );
      }
    }

    {
      // Block scope for state variables
      const { enableClientToNodeEncrypt, enableNodeToNodeEncrypt } = this.state;
      if (
        isDefinedNotNull(currentProvider) &&
        (enableClientToNodeEncrypt || enableNodeToNodeEncrypt)
      ) {
        const tlsCertOptions = [];
        if (this.props.type === 'Create') {
          tlsCertOptions.push(<option key={'cert-option-0'} value={''}>
            Create new certificate
          </option>);
        }

        if (!_.isEmpty(this.props.userCertificates.data)) {
          this.props.userCertificates.data.forEach((cert, index) => {
            if (this.props.type === 'Create') {
              const disableOnPremCustomCerts = currentProvider.code !== 'onprem' && cert.certType === 'CustomCertHostPath';
              tlsCertOptions.push(
                <option key={`cert-option-${index + 1}`} value={cert.uuid} disabled={disableOnPremCustomCerts}>
                  {cert.label}
                </option>
              );
            } else {
              const isCustomCertAndOnPrem = currentProvider.code === 'onprem' && cert.certType === 'CustomCertHostPath';
              tlsCertOptions.push(
                <option key={`cert-option-${index + 1}`} value={cert.uuid} disabled={!isCustomCertAndOnPrem}>
                  {cert.label}
                </option>
              );
            }
          });
        }

        const isSelectReadOnly = this.props.type === 'Edit' && currentProvider.code !== 'onprem';
        selectTlsCert = (
          <Field
            name={`${clusterType}.tlsCertificateId`}
            component={YBSelectWithLabel}
            options={tlsCertOptions}
            readOnlySelect={isSelectReadOnly}
            label="Root Certificate"
          />
        );
      }
    }

    if (
      isDefinedNotNull(currentProvider) &&
      (currentProvider.code === 'aws' ||
        currentProvider.code === 'gcp' ||
        currentProvider.code === 'azu')
    ) {
      // Assign public ip would be only enabled for primary and that same
      // value will be used for async as well.
      assignPublicIP = (
        <Field
          name={`${clusterType}.assignPublicIP`}
          component={YBToggle}
          isReadOnly={isFieldReadOnly}
          disableOnChange={disableToggleOnChange}
          checkedVal={this.state.assignPublicIP}
          onToggle={this.toggleAssignPublicIP}
          label="Assign Public IP"
          subLabel="Whether or not to assign a public IP."
        />
      );
    }
    // Only enable Time Sync Service toggle for AWS.
    if (isDefinedNotNull(currentProvider) && currentProvider.code === 'aws') {
      useTimeSync = (
        <Field
          name={`${clusterType}.useTimeSync`}
          component={YBToggle}
          isReadOnly={isFieldReadOnly}
          checkedVal={this.state.useTimeSync}
          onToggle={this.toggleUseTimeSync}
          label="Use AWS Time Sync"
          subLabel="Whether or not to use the Amazon Time Sync Service."
        />
      );
    }

    universeProviderList.unshift(<option key="" value=""></option>);

    let universeRegionList = [];
    if (self.state.providerSelected) {
      universeRegionList =
        cloud.regions.data &&
        cloud.regions.data.map(function (regionItem) {
          return { value: regionItem.uuid, label: regionItem.name };
        });
    }

    let universeInstanceTypeList = <option />;

    if (currentProviderCode === 'aws') {
      const optGroups =
        this.props.cloud.instanceTypes &&
        isNonEmptyArray(this.props.cloud.instanceTypes.data) &&
        this.props.cloud.instanceTypes.data.reduce(function (groups, it) {
          const prefix = it.instanceTypeCode.substr(0, it.instanceTypeCode.indexOf('.'));
          groups[prefix]
            ? groups[prefix].push(it.instanceTypeCode)
            : (groups[prefix] = [it.instanceTypeCode]);
          return groups;
        }, {});
      if (isNonEmptyObject(optGroups)) {
        universeInstanceTypeList = Object.keys(optGroups).map(function (key, idx) {
          return (
            <optgroup label={`${key.toUpperCase()} type instances`} key={key + idx}>
              {optGroups[key]
                .sort((a, b) => /\d+(?!\.)/.exec(a) - /\d+(?!\.)/.exec(b))
                .map((item, arrIdx) => (
                  <option key={idx + arrIdx} value={item}>
                    {item}
                  </option>
                ))}
            </optgroup>
          );
        });
      }
    } else if (currentProviderCode === 'kubernetes') {
      universeInstanceTypeList =
        cloud.instanceTypes.data &&
        cloud.instanceTypes.data.map(function (instanceTypeItem, idx) {
          return (
            <option
              key={instanceTypeItem.instanceTypeCode}
              value={instanceTypeItem.instanceTypeCode}
            >
              {instanceTypeItem.instanceTypeName || instanceTypeItem.instanceTypeCode} (
              {instanceTypeItem.numCores} {instanceTypeItem.numCores > 1 ? 'cores' : 'core'},{' '}
              {instanceTypeItem.memSizeGB}GB RAM)
            </option>
          );
        });
    } else {
      universeInstanceTypeList =
        cloud.instanceTypes.data &&
        cloud.instanceTypes.data.map(function (instanceTypeItem, idx) {
          return (
            <option
              key={instanceTypeItem.instanceTypeCode}
              value={instanceTypeItem.instanceTypeCode}
            >
              {instanceTypeItem.instanceTypeCode}
            </option>
          );
        });
    }

    let placementStatus = null;
    let placementStatusOnprem = null;
    const cluster =
      clusterType === 'primary'
        ? getPrimaryCluster(_.get(self.props, 'universe.universeConfigTemplate.data.clusters', []))
        : getReadOnlyCluster(
          _.get(self.props, 'universe.universeConfigTemplate.data.clusters', [])
        );
    const placementCloud = getPlacementCloud(cluster);
    const regionAndProviderDefined = isNonEmptyArray(formValues[clusterType]?.regionList)
      && isNonEmptyString(formValues[clusterType]?.provider);

    // For onprem provider type if numNodes < maxNumNodes then show the AZ error.
    if (
      self.props.universe.currentPlacementStatus && placementCloud
      && regionAndProviderDefined
    ) {
      placementStatus = (
        <AZPlacementInfo
          placementInfo={self.props.universe.currentPlacementStatus}
          placementCloud={placementCloud}
          providerCode={currentProvider.code}
        />
      );
    } else if (currentProvider?.code === 'onprem' 
      && !(this.state.numNodes <= this.state.maxNumNodes)
      && self.props.universe.currentPlacementStatus
      && isNonEmptyArray(formValues[clusterType].regionList)
      && isNonEmptyString(formValues[clusterType].provider)
    ) {
      placementStatusOnprem = (
        <AZPlacementInfo
          placementInfo={self.props.universe.currentPlacementStatus}
          providerCode={currentProvider.code}
        />
      );
    }
    const configTemplate = self.props.universe.universeConfigTemplate;
    const clusters = _.get(configTemplate, 'data.clusters', []);
    const showPlacementStatus =
      configTemplate && clusterType === 'primary'
        ? !!getPrimaryCluster(clusters)
        : clusterType === 'async'
          ? !!getReadOnlyCluster(clusters)
          : false;
    const azSelectorTable = (
      <div>
        <AZSelectorTable
          {...this.props}
          clusterType={clusterType}
          numNodesChangedViaAzList={this.numNodesChangedViaAzList}
          minNumNodes={this.state.replicationFactor}
          maxNumNodes={this.state.maxNumNodes}
          currentProvider={this.getCurrentProvider(currentProviderUUID)}
          isKubernetesUniverse={this.state.isKubernetesUniverse}
        />
        {showPlacementStatus && placementStatus}
      </div>
    );

    if (clusterType === 'primary') {
      gflagArray = (
        <Row>
          <Col md={12}>
            <h4>G-Flags</h4>
          </Col>
          <Col md={6}>
            <FieldArray
              component={GFlagArrayComponent}
              name={`${clusterType}.masterGFlags`}
              flagType="master"
              operationType="Create"
              isReadOnly={isFieldReadOnly}
            />
          </Col>
          <Col md={6}>
            <FieldArray
              component={GFlagArrayComponent}
              name={`${clusterType}.tserverGFlags`}
              flagType="tserver"
              operationType="Create"
              isReadOnly={isFieldReadOnly}
            />
          </Col>
        </Row>
      );
      tagsArray = (
        <Row>
          <Col md={12}>
            <h4>User Tags</h4>
          </Col>
          <Col md={6}>
            <FieldArray
              component={GFlagArrayComponent}
              name={`${clusterType}.instanceTags`}
              flagType="tag"
              operationType="Create"
              isReadOnly={false}
            />
          </Col>
        </Row>
      );
    }

    const softwareVersionOptions = softwareVersions.map((item, idx) => (
      <option key={idx} value={item}>
        {item}
      </option>
    ));

    let accessKeyOptions = (
      <option key={1} value={this.state.accessKeyCode}>
        {this.state.accessKeyCode}
      </option>
    );
    if (_.isObject(accessKeys) && isNonEmptyArray(accessKeys.data)) {
      accessKeyOptions = accessKeys.data
        .filter((key) => key.idKey.providerUUID === currentProviderUUID)
        .map((item, idx) => (
          <option key={idx} value={item.idKey.keyCode}>
            {item.idKey.keyCode}
          </option>
        ));
    }
    let universeNameField = <span />;
    if (clusterType === 'primary') {
      universeNameField = (
        <Field
          name={`${clusterType}.universeName`}
          type="text"
          normalize={trimSpecialChars}
          component={YBTextInputWithLabel}
          label="Name"
          isReadOnly={isFieldReadOnly}
        />
      );
    }

    const portValidation = (value) => (value && value < 65536 ? undefined : 'Invalid Port');

    return (
      <div>
        <div className="form-section" data-yb-section="cloud-config">
          <Row>
            <Col md={6}>
              <h4 style={{ marginBottom: 40 }}>Cloud Configuration</h4>
              {this.state.isAZUpdating}
              <div className="form-right-aligned-labels">
                {universeNameField}
                {providerNameField}
                <Field
                  name={`${clusterType}.regionList`}
                  component={YBMultiSelectWithLabel}
                  options={universeRegionList}
                  label="Regions"
                  data-yb-field="regions"
                  isMulti={true}
                  selectValChanged={this.regionListChanged}
                  providerSelected={currentProviderUUID}
                />
                {clusterType === 'async'
                  ? [
                    <Field
                      key="numNodes"
                      name={`${clusterType}.numNodes`}
                      type="text"
                      component={YBControlledNumericInputWithLabel}
                      className={
                        getPromiseState(this.props.universe.universeConfigTemplate).isLoading()
                          ? 'readonly'
                          : ''
                      }
                      data-yb-field="nodes"
                      label={this.state.isKubernetesUniverse ? 'Pods' : 'Nodes'}
                      onInputChanged={this.numNodesChanged}
                      onLabelClick={this.numNodesClicked}
                      val={this.state.numNodes}
                      minVal={Number(this.state.replicationFactor)}
                    />,
                    <Field
                      key="replicationFactor"
                      name={`${clusterType}.replicationFactor`}
                      type="text"
                      component={YBRadioButtonBarWithLabel}
                      options={[1, 2, 3, 4, 5, 6, 7]}
                      label="Replication Factor"
                      initialValue={this.state.replicationFactor}
                      onSelect={this.replicationFactorChanged}
                      isReadOnly={isFieldReadOnly}
                    />
                  ]
                  : null}
              </div>

              {clusterType !== 'async' && (
                <Row>
                  <div className="form-right-aligned-labels">
                    <Col lg={5}>
                      <Field
                        name={`${clusterType}.numNodes`}
                        type="text"
                        component={YBControlledNumericInputWithLabel}
                        className={
                          getPromiseState(this.props.universe.universeConfigTemplate).isLoading()
                            ? 'readonly'
                            : ''
                        }
                        label={this.state.isKubernetesUniverse ? 'Pods' : 'Nodes'}
                        onInputChanged={this.numNodesChanged}
                        onLabelClick={this.numNodesClicked}
                        val={this.state.numNodes}
                        minVal={Number(this.state.replicationFactor)}
                      />
                    </Col>
                    <Col lg={7} className="button-group-row">
                      <Field
                        name={`${clusterType}.replicationFactor`}
                        type="text"
                        component={YBRadioButtonBarWithLabel}
                        options={[1, 3, 5, 7]}
                        label="Replication Factor"
                        initialValue={this.state.replicationFactor}
                        onSelect={this.replicationFactorChanged}
                        isReadOnly={isFieldReadOnly}
                      />
                    </Col>
                  </div>
                </Row>
              )}
            </Col>
            <Col md={6} className={'universe-az-selector-container'}>
              {placementStatusOnprem  || regionAndProviderDefined && azSelectorTable}
            </Col>
          </Row>
        </div>
        <div className="form-section" data-yb-section="instance-config">
          <Row>
            <Col md={12}>
              <h4>Instance Configuration</h4>
            </Col>
          </Row>
          <Row>
            <Col sm={12} md={12} lg={6}>
              <div className="form-right-aligned-labels">
                <Field
                  name={`${clusterType}.instanceType`}
                  component={YBSelectWithLabel}
                  label="Instance Type"
                  options={universeInstanceTypeList}
                  onInputChanged={this.instanceTypeChanged}
                />
              </div>
            </Col>
            <Col sm={12} md={12} lg={6}>
              {deviceDetail && (
                <div className="form-right-aligned-labels">
                  <div className="form-inline-controls">
                    <div
                      className="form-group universe-form-instance-info"
                      data-yb-field="volumn-info"
                    >
                      <label className="form-item-label form-item-label-shrink">Volume Info</label>
                      {deviceDetail}
                    </div>
                  </div>
                  <div className="form-inline-controls">
                    <div className="form-group universe-form-instance-info">
                      {storageTypeSelector}
                    </div>
                  </div>
                  <div className="form-inline-controls">
                    <div className="form-group universe-form-instance-info">{iopsField}</div>
                  </div>
                </div>
              )}
            </Col>
            <Col sm={12} md={12} lg={6}>
              <div className="form-right-aligned-labels">
                {selectTlsCert}
                {assignPublicIP}
                {useTimeSync}
                {enableYSQL}
                {enableYEDIS}
                {enableNodeToNodeEncrypt}
                {enableClientToNodeEncrypt}
                {enableEncryptionAtRest}
                <Field name={`${clusterType}.mountPoints`} component={YBTextInput} type="hidden" />
              </div>
            </Col>
            <Col sm={12} md={6} lg={4}>
              <div className="form-right-aligned-labels right-side-form-field">
                <Row>{selectEncryptionAtRestConfig}</Row>
              </div>
            </Col>
          </Row>
        </div>
        <div className="form-section" data-yb-section="advanced">
          <Row>
            <Col md={12}>
              <h4>Advanced</h4>
            </Col>
            <Col sm={5} md={4}>
              <div className="form-right-aligned-labels">
                <Field
                  name={`${clusterType}.ybSoftwareVersion`}
                  component={YBSelectWithLabel}
                  options={softwareVersionOptions}
                  label="DB Version"
                  onInputChanged={this.softwareVersionChanged}
                  readOnlySelect={isFieldReadOnly}
                />
              </div>
            </Col>
            {!this.state.isKubernetesUniverse && (
              <Col lg={4}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.accessKeyCode`}
                    type="select"
                    component={YBSelectWithLabel}
                    label="Access Key"
                    onInputChanged={this.accessKeyChanged}
                    options={accessKeyOptions}
                    readOnlySelect={isFieldReadOnly}
                  />
                </div>
              </Col>
            )}
          </Row>
          {isDefinedNotNull(currentProvider) && currentProvider.code === 'aws' && (
            <Row>
              <Col sm={5} md={4}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.awsArnString`}
                    type="text"
                    component={YBTextInputWithLabel}
                    label="Instance Profile ARN"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
            </Row>
          )}
          {isDefinedNotNull(currentProvider) && currentProvider.code === 'kubernetes' && (
            <Row>
              <Col md={12}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.enableIPV6`}
                    component={YBToggle}
                    isReadOnly={isFieldReadOnly}
                    disableOnChange={disableToggleOnChange}
                    checkedVal={this.state.enableIPV6}
                    onToggle={this.toggleEnableIPV6}
                    label="Enable IPV6"
                    subLabel="Whether or not to enable IPV6."
                  />
                </div>
              </Col>
            </Row>
          )}
          {isDefinedNotNull(currentProvider) && currentProvider.code !== 'kubernetes' && (
            <Row>
              <Col md={12}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.customizePorts`}
                    component={YBToggle}
                    defaultChecked={false}
                    disableOnChange={disableToggleOnChange}
                    checkedVal={this.state.customizePorts}
                    onToggle={this.toggleCustomizePorts}
                    label="Override Deployment Ports"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
            </Row>
          )}
          {this.state.customizePorts && (
            <Row>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.masterHttpPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="Master HTTP Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.masterRpcPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="Master RPC Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
            </Row>
          )}
          {this.state.customizePorts && (
            <Row>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.tserverHttpPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="Tserver HTTP Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.tserverRpcPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="Tserver RPC Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
            </Row>
          )}
          {this.state.customizePorts && (
            <Row>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.yqlHttpPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="YCQL HTTP Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.yqlRpcPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="YCQL RPC Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
            </Row>
          )}
          {this.state.customizePorts && this.state.enableYSQL && (
            <Row>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.ysqlHttpPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="YSQL HTTP Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.ysqlRpcPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="YSQL RPC Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
            </Row>
          )}
          {this.state.customizePorts && this.state.enableYEDIS && (
            <Row>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.redisHttpPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="Yedis HTTP Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
              <Col sm={3}>
                <div className="form-right-aligned-labels">
                  <Field
                    name={`${clusterType}.redisRpcPort`}
                    type="text"
                    component={YBTextInputWithLabel}
                    normalize={normalizeToValidPort}
                    validate={portValidation}
                    label="Yedis RPC Port"
                    isReadOnly={isFieldReadOnly}
                  />
                </div>
              </Col>
            </Row>
          )}
        </div>
        <div className="form-section" data-yb-section="g-flags">
          {gflagArray}
        </div>
        {currentProviderCode === 'aws' && clusterType === 'primary' && (
          <div className="form-section no-border">{tagsArray}</div>
        )}
      </div>
    );
  }
}
