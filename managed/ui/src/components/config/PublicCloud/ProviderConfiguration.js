// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { getPromiseState } from '../../../utils/PromiseUtils';
import {
  isNonEmptyObject,
  isDefinedNotNull,
  isNonEmptyArray,
  isNonEmptyString
} from '../../../utils/ObjectUtils';
import { withRouter } from 'react-router';
import _ from 'lodash';
import ProviderResultView from './views/ProviderResultView';
import ProviderBootstrapView from './views/ProviderBootstrapView';
import AWSProviderInitView from './views/AWSProviderInitView';
import GCPProviderInitView from './views/GCPProviderInitView';
import { AzureProviderInitView } from './views/AzureProviderInitView';
import { YBLoading } from '../../common/indicators';

class ProviderConfiguration extends Component {
  constructor(props) {
    super(props);
    this.state = {
      currentView: '',
      currentTaskUUID: '',
      refreshSucceeded: false
    };
  }

  getInitView = () => {
    switch (this.props.providerType) {
      case 'aws': return <AWSProviderInitView {...this.props} />;
      case 'gcp': return <GCPProviderInitView {...this.props} />;
      case 'azu': return <AzureProviderInitView createAzureProvider={this.props.createAzureProvider} />;
      default: return <div>Unknown provider type <strong>{this.props.providerType}</strong></div>;
    }
  };

  componentDidMount() {
    const {
      configuredProviders,
      tasks: { customerTaskList },
      providerType,
      getCurrentTaskData,
      fetchHostInfo,
      fetchCustomerTasksList
    } = this.props;
    const currentProvider = configuredProviders.data.find(
      (provider) => provider.code === providerType
    );

    fetchHostInfo();
    fetchCustomerTasksList();

    if (
      getPromiseState(configuredProviders).isLoading() ||
      getPromiseState(configuredProviders).isInit()
    ) {
      this.setState({ currentView: 'loading' });
    } else {
      this.setState({ currentView: isNonEmptyObject(currentProvider) ? 'result' : 'init' });
      let currentProviderTask = null;
      if (
        customerTaskList &&
        isNonEmptyArray(customerTaskList.data) &&
        isDefinedNotNull(currentProvider)
      ) {
        currentProviderTask = customerTaskList.data.find(
          (task) => task.targetUUID === currentProvider.uuid
        );
        if (isDefinedNotNull(currentProviderTask) && currentProviderTask.status !== 'Success') {
          getCurrentTaskData(currentProviderTask.id);
          this.setState({ currentTaskUUID: currentProviderTask.id, currentView: 'bootstrap' });
        }
      }
    }
  }

  componentDidUpdate(prevProps) {
    const {
      configuredProviders,
      cloud: { bootstrapProvider },
      cloudBootstrap,
      cloudBootstrap: {
        data: { type },
        promiseState
      },
      tasks: { customerTaskList },
      providerType
    } = this.props;
    const { refreshing } = this.state;
    if (refreshing && type === 'initialize' && !promiseState.isLoading()) {
      this.setState({ refreshing: false });
    }
    let currentProvider = null;
    if (configuredProviders.data) {
      currentProvider = configuredProviders.data.find((provider) => provider.code === providerType);
    }
    let currentProviderTask = null;
    if (!_.isEqual(configuredProviders.data, prevProps.configuredProviders.data)) {
      this.setState({ currentView: isNonEmptyObject(currentProvider) ? 'result' : 'init' });
    }

    if (
      getPromiseState(configuredProviders).isEmpty() &&
      !getPromiseState(prevProps.configuredProviders).isEmpty()
    ) {
      this.setState({ currentView: 'init' });
    }

    if (
      customerTaskList &&
      isNonEmptyArray(customerTaskList.data) &&
      isNonEmptyObject(currentProvider) &&
      isNonEmptyArray(prevProps.tasks.customerTaskList.data) &&
      prevProps.tasks.customerTaskList.data.length === 0
    ) {
      currentProviderTask = customerTaskList.data.find(
        (task) => task.targetUUID === currentProvider.uuid
      );
      if (currentProviderTask) {
        this.props.getCurrentTaskData(currentProviderTask.id);
        if (isDefinedNotNull(currentProviderTask) && currentProviderTask.status !== 'Success') {
          this.setState({ currentTaskUUID: currentProviderTask.id, currentView: 'bootstrap' });
        }
      }
    }

    // If Provider Bootstrap task has started, go to provider bootstrap view.
    if (
      getPromiseState(prevProps.cloud.bootstrapProvider).isLoading() &&
      getPromiseState(bootstrapProvider).isSuccess()
    ) {
      this.setState({ currentTaskUUID: bootstrapProvider.data.taskUUID, currentView: 'bootstrap' });
      this.props.getCurrentTaskData(bootstrapProvider.data.taskUUID);
    }

    if (
      type === 'initialize' &&
      cloudBootstrap.promiseState.name === 'SUCCESS' &&
      prevProps.cloudBootstrap.promiseState.name === 'LOADING'
    ) {
      this.setState({ refreshSucceeded: true });
    }
  }

  getResultView = () => {
    const {
      configuredProviders,
      modal: { visibleModal },
      configuredRegions,
      universeList,
      accessKeys,
      hideDeleteProviderModal,
      initializeProvider,
      showDeleteProviderModal,
      deleteProviderConfig,
      providerType,
      hostInfo
    } = this.props;
    const currentProvider =
      configuredProviders.data.find((provider) => provider.code === providerType) || {};
    let keyPairName = 'Not Configured';
    if (isDefinedNotNull(accessKeys) && isNonEmptyArray(accessKeys.data)) {
      const currentAccessKey = accessKeys.data.find(
        (accessKey) => accessKey.idKey.providerUUID === currentProvider.uuid
      );
      if (isDefinedNotNull(currentAccessKey)) {
        keyPairName = currentAccessKey.idKey.keyCode;
      }
    }
    let regions = [];
    if (isNonEmptyObject(currentProvider)) {
      if (isNonEmptyArray(configuredRegions.data)) {
        regions = configuredRegions.data.filter(
          (region) => region.provider.uuid === currentProvider.uuid
        );
      }
      const providerInfo = [
        { name: 'Name', data: currentProvider.name },
        { name: 'Provider UUID', data: currentProvider.uuid },
        { name: 'SSH Key', data: keyPairName }
      ];
      if (
        currentProvider.code === 'aws' &&
        isNonEmptyString(currentProvider.config.AWS_HOSTED_ZONE_ID)
      ) {
        providerInfo.push({
          name: 'Hosted Zone ID',
          data: currentProvider.config.AWS_HOSTED_ZONE_ID
        });
      }
      if (
        currentProvider.code === 'aws' &&
        isNonEmptyString(currentProvider.config.AWS_HOSTED_ZONE_NAME)
      ) {
        providerInfo.push({
          name: 'Hosted Zone Name',
          data: currentProvider.config.AWS_HOSTED_ZONE_NAME
        });
      }
      if (isNonEmptyObject(hostInfo)) {
        if (currentProvider.code === 'aws' && isNonEmptyObject(hostInfo['aws'])) {
          const awsHostInfo = hostInfo['aws'];
          if (isNonEmptyString(awsHostInfo['region'])) {
            providerInfo.push({ name: 'Host Region', data: awsHostInfo['region'] });
          }
          if (isNonEmptyString(awsHostInfo['vpc-id'])) {
            providerInfo.push({ name: 'Host VPC ID', data: awsHostInfo['vpc-id'] });
          }
          if (isNonEmptyString(awsHostInfo['privateIp'])) {
            providerInfo.push({ name: 'Host Private IP', data: awsHostInfo['privateIp'] });
          }
        }
        if (currentProvider.code === 'gcp' && isNonEmptyObject(hostInfo['gcp'])) {
          const gcpHostInfo = hostInfo['gcp'];
          if (isNonEmptyString(gcpHostInfo['network'])) {
            providerInfo.push({ name: 'Host Network', data: gcpHostInfo['network'] });
          }
          if (isNonEmptyString(gcpHostInfo['project'])) {
            providerInfo.push({ name: 'Host Project', data: gcpHostInfo['project'] });
          }
        }
      }
      let universeExistsForProvider = false;
      if (
        getPromiseState(configuredProviders).isSuccess() &&
        getPromiseState(universeList).isSuccess()
      ) {
        universeList.data.forEach(function (universeItem) {
          universeItem.universeDetails.clusters.forEach(function (cluster) {
            if (cluster.userIntent.provider === currentProvider.uuid) {
              universeExistsForProvider = true;
            }
          });
        });
      }
      let currentModal = '';
      switch (providerType) {
        case 'aws':
          currentModal = 'deleteAWSProvider';
          break;
        case 'gcp':
          currentModal = 'deleteGCPProvider';
          break;
        case 'azu':
          currentModal = 'deleteAzureProvider';
          break;
        default:
          break;
      }
      const deleteButtonDisabled = universeExistsForProvider;
      return (
        <ProviderResultView
          regions={regions}
          providerInfo={providerInfo}
          currentProvider={currentProvider}
          initializeMetadata={initializeProvider}
          showDeleteProviderModal={showDeleteProviderModal}
          visibleModal={visibleModal}
          deleteProviderConfig={deleteProviderConfig}
          hideDeleteProviderModal={hideDeleteProviderModal}
          currentModal={currentModal}
          providerType={providerType}
          deleteButtonDisabled={deleteButtonDisabled}
          refreshSucceeded={this.state.refreshSucceeded}
        />
      );
    }
  };

  getBootstrapView = () => {
    const {
      configuredProviders,
      reloadCloudMetadata,
      cloud: { createProvider },
      providerType,
      showDeleteProviderModal,
      modal: { visibleModal },
      deleteProviderConfig,
      hideDeleteProviderModal
    } = this.props;
    let currentModal = '';
    switch (providerType) {
      case 'aws':
        currentModal = 'deleteAWSProvider';
        break;
      case 'gcp':
        currentModal = 'deleteGCPProvider';
        break;
      case 'azu':
        currentModal = 'deleteAzureProvider';
        break;
      default:
        break;
    }

    const currentConfiguredProvider = configuredProviders.data.find(
      (provider) => provider.code === providerType
    );
    let provider = {};
    if (isNonEmptyObject(createProvider.data)) {
      provider = createProvider.data;
    } else if (isNonEmptyObject(currentConfiguredProvider)) {
      provider = currentConfiguredProvider;
    }

    return (
      <ProviderBootstrapView
        taskUUIDs={[this.state.currentTaskUUID]}
        currentProvider={provider}
        showDeleteProviderModal={showDeleteProviderModal}
        visibleModal={visibleModal}
        reloadCloudMetadata={reloadCloudMetadata}
        providerType={providerType}
        currentModal={currentModal}
        deleteProviderConfig={deleteProviderConfig}
        hideDeleteProviderModal={hideDeleteProviderModal}
      />
    );
  };

  render() {
    let currentProviderView = <span />;
    if (this.state.currentView === 'init') {
      currentProviderView = this.getInitView();
    } else if (this.state.currentView === 'loading') {
      currentProviderView = <YBLoading />;
    } else if (this.state.currentView === 'bootstrap') {
      currentProviderView = this.getBootstrapView();
    } else if (this.state.currentView === 'result') {
      currentProviderView = this.getResultView();
    }
    return <div className="provider-config-container">{currentProviderView}</div>;
  }
}

export default withRouter(ProviderConfiguration);
