// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { getPromiseState } from '../../../../utils/PromiseUtils';
import { YBLoadingCircleIcon } from '../../../common/indicators';
import { withRouter } from 'react-router';
import { isNonEmptyArray, isDefinedNotNull } from '../../../../utils/ObjectUtils';
import ListKubernetesConfigurations from './ListKubernetesConfigurations';
import CreateKubernetesConfigurationContainer from './CreateKubernetesConfigurationContainer';
import { KUBERNETES_PROVIDERS } from '../../../../config';

const PROVIDER_TYPE = 'kubernetes';

class KubernetesProviderConfiguration extends Component {
  constructor(props) {
    super(props);
    this.state = {
      listView: true
    };
  }

  toggleListView = (value) => {
    if (typeof value === typeof true) {
      this.setState({ listView: value });
    } else {
      this.setState({ listView: !this.state.listView });
    }
  };

  render() {
    const {
      providers,
      regions,
      universeList,
      type,
      params: { uuid }
    } = this.props;

    if (getPromiseState(providers).isLoading() || getPromiseState(providers).isInit()) {
      return <YBLoadingCircleIcon size="medium" />;
    }

    const kubernetesRegions = regions.data.filter(
      (region) => region.provider.code === PROVIDER_TYPE
    );

    const configuredProviderData = kubernetesRegions
      .map((region) => {
        const providerData = providers.data.find((p) => p.uuid === region.provider.uuid);

        // If the type is Tanzu we don't want to include other k8s configs and vice versa.
        if (
          !isDefinedNotNull(providerData) ||
          (type === 'tanzu' && providerData.config['KUBECONFIG_PROVIDER'] !== type) ||
          (type === 'k8s' && providerData.config['KUBECONFIG_PROVIDER'] === 'tanzu')
        ) {
          return null;
        }
        const providerTypeMetadata = KUBERNETES_PROVIDERS.find(
          (providerType) => providerType.code === providerData.config['KUBECONFIG_PROVIDER']
        );
        return {
          uuid: region.provider.uuid,
          name: region.provider.name,
          region: region.name,
          zones: region.zones.map((zone) => zone.name).join(', '),
          configPath: providerData.config['KUBECONFIG'],
          namespace: providerData.config['KUBECONFIG_NAMESPACE'],
          serviceAccount: providerData.config['KUBECONFIG_SERVICE_ACCOUNT'],
          type: providerTypeMetadata && providerTypeMetadata.name
        };
      })
      .filter(Boolean);

    if (this.state.listView && isNonEmptyArray(configuredProviderData)) {
      return (
        <ListKubernetesConfigurations
          providers={configuredProviderData}
          onCreate={this.toggleListView}
          activeProviderUUID={uuid}
          universeList={universeList}
          deleteProviderConfig={this.props.deleteProviderConfig}
          closeModal={this.props.closeModal}
          showDeleteConfirmationModal={this.props.showDeleteConfirmationModal}
          modal={this.props.modal}
          type={type}
        />
      );
    } else {
      return (
        <CreateKubernetesConfigurationContainer
          hasConfigs={isNonEmptyArray(configuredProviderData)}
          toggleListView={this.toggleListView}
          type={type}
        />
      );
    }
  }
}

export default withRouter(KubernetesProviderConfiguration);
