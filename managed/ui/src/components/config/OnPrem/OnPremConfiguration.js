// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Alert } from 'react-bootstrap';
import _ from 'lodash';
import {
  isValidObject,
  isDefinedNotNull,
  isNonEmptyArray,
  isEmptyObject,
  isNonEmptyObject
} from '../../../utils/ObjectUtils';
import {
  OnPremConfigWizardContainer,
  OnPremConfigJSONContainer,
  OnPremSuccessContainer
} from '../../config';
import { YBButton } from '../../common/forms/fields';
import emptyDataCenterConfig from '../templates/EmptyDataCenterConfig.json';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { YBLoading } from '../../common/indicators';
import './OnPremConfiguration.scss';

const PROVIDER_TYPE = 'onprem';
const initialState = {
  failedBootstrapMessage: null,
  isEditingProvider: false,
  isJsonEntry: false,
  isAdditionalHostOptionsOpen: false,
  configJsonVal: JSON.stringify(JSON.parse(JSON.stringify(emptyDataCenterConfig)), null, 2),
  bootstrapSteps: [
    { type: 'provider', name: 'Create Provider', status: 'Initializing' },
    { type: 'instanceType', name: 'Create Instance Types', status: 'Initializing' },
    { type: 'region', name: 'Create Regions', status: 'Initializing' },
    { type: 'zones', name: 'Create Zones', status: 'Initializing' },
    { type: 'node', name: 'Create Node Instances', status: 'Initializing' },
    { type: 'accessKey', name: 'Create Access Keys', status: 'Initializing' }
  ],
  regionsMap: {},
  providerUUID: null,
  numZones: 0,
  numNodesConfigured: 0,
  numRegions: 0,
  numInstanceTypes: 0,
  numInstanceTypesConfigured: 0
};

export default class OnPremConfiguration extends Component {
  constructor(props) {
    super(props);
    this.state = _.clone(initialState, true);
    this.props.resetConfigForm();
  }

  serializeStringToJson = (payloadString) => {
    if (!_.isString(payloadString)) {
      return payloadString;
    }
    let jsonPayload = {};
    try {
      jsonPayload = JSON.parse(payloadString);
    } catch (e) {
      // Handle case where private key contains newline characters and hence is not valid json
      const jsonPayloadTokens = payloadString.split('"privateKeyContent":');
      const privateKeyBlob = jsonPayloadTokens[1].split('}')[0].trim();
      const privateKeyString = privateKeyBlob.replace(/\n/g, '\\n');
      const newJsonString =
        jsonPayloadTokens[0] + '"privateKeyContent": ' + privateKeyString + '\n}\n}';
      jsonPayload = JSON.parse(newJsonString);
    }
    // Add sshUser to Node Payload, if not added
    jsonPayload.nodes.map((node) => (node.sshUser = node.sshUser || jsonPayload.key.sshUser));
    return jsonPayload;
  };

  showEditProviderForm = () => {
    this.setState({ isEditingProvider: true });
  };

  UNSAFE_componentWillReceiveProps(nextProps) {
    const {
      cloudBootstrap: {
        data: { response, type },
        error,
        promiseState
      },
      accessKeys,
      cloud: { providers }
    } = nextProps;
    let onPremAccessKey = {};
    if (isNonEmptyArray(accessKeys.data) && isNonEmptyArray(providers.data)) {
      const onPremProvider = providers.data.find((provider) => {
        return provider.code === 'onprem';
      });
      if (isNonEmptyObject(onPremProvider)) {
        onPremAccessKey = accessKeys.data.find(function (key) {
          return key.idKey.providerCode === onPremProvider.uuid;
        });
      }
    }
    const bootstrapSteps = this.state.bootstrapSteps;
    const currentStepIndex = bootstrapSteps.findIndex((step) => step.type === type);
    if (currentStepIndex !== -1) {
      if (promiseState.isLoading()) {
        bootstrapSteps[currentStepIndex].status = 'Running';
      } else {
        bootstrapSteps[currentStepIndex].status = error ? 'Error' : 'Success';
      }
      this.setState({ bootstrapSteps: bootstrapSteps });
    }

    // check if provider successfully created but some bootstrap step failed
    // if so - proceed to the success state but with an error message of the failed step
    if (bootstrapSteps[0].status === 'Success' && promiseState.isError()) {
      this.setState({
        ..._.clone(initialState),
        failedBootstrapMessage: `${bootstrapSteps[currentStepIndex].name} bootstrap step failed. ${error}`
      });
      this.props.onPremConfigSuccess();
    }

    if (isValidObject(response)) {
      const { isEditingProvider, numRegions } = this.state;
      const payloadString = _.clone(this.state.configJsonVal);
      const config = this.serializeStringToJson(payloadString);
      const numZones = config.regions.reduce((total, region) => {
        return total + region.zones.length;
      }, 0);
      switch (type) {
        case 'provider':
          // Launch configuration of instance types
          this.setState({
            regionsMap: {},
            providerUUID: response.uuid,
            numRegions: config.regions.length,
            numInstanceTypes: config.instanceTypes.length,
            numZones: numZones,
            numNodesConfigured: 0,
            numInstanceTypesConfigured: 0
          });
          bootstrapSteps[currentStepIndex + 1].status = 'Running';
          this.props.createOnPremInstanceTypes(
            PROVIDER_TYPE,
            response.uuid,
            config,
            isEditingProvider
          );
          break;
        case 'instanceType':
          // Launch configuration of regions
          let numInstanceTypesConfigured = this.state.numInstanceTypesConfigured;
          numInstanceTypesConfigured++;
          this.setState({ numInstanceTypesConfigured: numInstanceTypesConfigured });
          if (numInstanceTypesConfigured === this.state.numInstanceTypes) {
            bootstrapSteps[currentStepIndex + 1].status = 'Running';
            if (isEditingProvider && numRegions === 0) {
              this.resetEdit();
            } else {
              this.props.createOnPremRegions(this.state.providerUUID, config, isEditingProvider);
            }
          }
          break;
        case 'region':
          // Update regionsMap until done
          const regionsMap = this.state.regionsMap;
          regionsMap[response.code] = response.uuid;
          this.setState({ regionsMap: regionsMap });
          // Launch configuration of zones once all regions are bootstrapped
          if (Object.keys(regionsMap).length === this.state.numRegions) {
            bootstrapSteps[currentStepIndex + 1].status = 'Running';
            this.props.createOnPremZones(
              this.state.providerUUID,
              regionsMap,
              config,
              isEditingProvider
            );
          }
          break;
        case 'zones':
          // Update zonesMap until done
          const zonesMap = {};
          Object.keys(response).forEach(
            (zoneCode) => (zonesMap[zoneCode] = response[zoneCode].uuid)
          );
          bootstrapSteps[currentStepIndex + 1].status = 'Running';
          // If Edit Case, then jump to success
          if (isEditingProvider) {
            this.resetEdit();
          } else if (isNonEmptyArray(config.nodes)) {
            this.props.createOnPremNodes(zonesMap, config);
          } else {
            this.props.createOnPremAccessKeys(
              this.state.providerUUID,
              this.state.regionsMap,
              config
            );
          }
          break;
        case 'node':
          // Update numNodesConfigured until done
          const numNodesConfigured = this.state.numNodesConfigured + Object.keys(response).length;
          this.setState({ numNodesConfigured: numNodesConfigured });
          // Launch configuration of access keys once all node instances are bootstrapped
          if (numNodesConfigured === config.nodes.length) {
            bootstrapSteps[currentStepIndex + 1].status = 'Running';
            if (
              config.key &&
              _.isString(config.key.privateKeyContent) &&
              isEmptyObject(onPremAccessKey) &&
              this.state.providerUUID
            ) {
              this.props.createOnPremAccessKeys(
                this.state.providerUUID,
                this.state.regionsMap,
                config
              );
            }
          }
          break;
        case 'accessKey':
          this.setState(_.clone(initialState, true));
          this.props.onPremConfigSuccess();
          break;
        default:
          break;
      }
    }
  }

  resetEdit = () => {
    this.setState(_.clone(initialState, true));
    this.props.resetConfigForm();
    this.props.onPremConfigSuccess();
  };

  toggleJsonEntry = () => {
    this.setState({ isJsonEntry: !this.state.isJsonEntry });
  };

  toggleAdditionalOptionsModal = () => {
    this.setState({ isAdditionalHostOptionsOpen: !this.state.isAdditionalHostOptionsOpen });
  };

  updateConfigJsonVal = (newConfigJsonVal) => {
    this.setState({ configJsonVal: newConfigJsonVal });
  };

  submitJson = () => {
    if (this.state.isJsonEntry) {
      this.props.createOnPremProvider(
        PROVIDER_TYPE,
        this.serializeStringToJson(this.state.configJsonVal)
      );
    }
  };

  cancelEdit = () => {
    this.setState({ isEditingProvider: false });
  };

  submitEditProvider = (payloadData) => {
    const {
      cloud: { providers }
    } = this.props;
    const self = this;
    const currentProvider = providers.data.find((provider) => provider.code === 'onprem');
    let totalNumRegions = 0;
    let totalNumInstances = 0;
    let totalNumZones = 0;
    payloadData.regions.forEach(function (region) {
      if (region.isBeingEdited) {
        totalNumRegions++;
        totalNumZones += region.zones.length;
      }
    });
    payloadData.instanceTypes.forEach(function (instanceType) {
      if (instanceType.isBeingEdited) {
        totalNumInstances++;
      }
    });
    if (totalNumInstances === 0 && totalNumRegions === 0) {
      this.setState({ configJsonVal: payloadData, isEditingProvider: false });
    } else {
      this.setState(
        {
          numRegions: totalNumRegions,
          numZones: totalNumZones,
          numInstanceTypes: totalNumInstances,
          configJsonVal: payloadData,
          providerUUID: currentProvider.uuid
        },
        function () {
          if (totalNumInstances > 0) {
            self.props.createOnPremInstanceTypes(
              currentProvider.code,
              currentProvider.uuid,
              payloadData,
              true
            );
          } else {
            // If configuring only regions, then directly jump to region configure
            self.props.createOnPremRegions(currentProvider.uuid, payloadData, true);
          }
        }
      );
    }
  };

  submitWizardJson = (payloadData) => {
    this.setState({ configJsonVal: payloadData });
    this.props.createOnPremProvider(PROVIDER_TYPE, payloadData);
  };

  render() {
    const { configuredProviders, params } = this.props;
    const { configJsonVal, isEditingProvider } = this.state;
    if (
      getPromiseState(configuredProviders).isInit() ||
      getPromiseState(configuredProviders).isError()
    ) {
      return <span />;
    } else if (getPromiseState(configuredProviders).isLoading()) {
      return <YBLoading />;
    } else if (getPromiseState(configuredProviders).isSuccess()) {
      const providerFound = configuredProviders.data.find((provider) => provider.code === 'onprem');
      if (isDefinedNotNull(providerFound)) {
        if (this.state.isEditingProvider) {
          return (
            <OnPremConfigWizardContainer
              submitWizardJson={this.submitWizardJson}
              isEditProvider={isEditingProvider}
              submitEditProvider={this.submitEditProvider}
              cancelEdit={this.cancelEdit}
            />
          );
        }
        return (
          <>
            {this.state.failedBootstrapMessage && (
              <Alert
                bsStyle="danger"
                onDismiss={() => this.setState({ failedBootstrapMessage: null })}
              >
                {this.state.failedBootstrapMessage}
              </Alert>
            )}
            <OnPremSuccessContainer
              showEditProviderForm={this.showEditProviderForm}
              params={params}
            />
          </>
        );
      }
    }
    const switchToJsonEntry = (
      <YBButton
        btnText={'Switch to JSON View'}
        btnClass={'btn btn-default pull-left'}
        onClick={this.toggleJsonEntry}
      />
    );
    const switchToWizardEntry = (
      <YBButton
        btnText={'Switch to Wizard View'}
        btnClass={'btn btn-default pull-left'}
        onClick={this.toggleJsonEntry}
      />
    );
    let ConfigurationDataForm = (
      <OnPremConfigWizardContainer
        switchToJsonEntry={switchToJsonEntry}
        submitWizardJson={this.submitWizardJson}
      />
    );
    if (this.state.isJsonEntry) {
      ConfigurationDataForm = (
        <OnPremConfigJSONContainer
          updateConfigJsonVal={this.updateConfigJsonVal}
          configJsonVal={
            _.isString(configJsonVal)
              ? configJsonVal
              : JSON.stringify(JSON.parse(JSON.stringify(configJsonVal)), null, 2)
          }
          switchToWizardEntry={switchToWizardEntry}
          submitJson={this.submitJson}
        />
      );
    }
    return <div className="on-prem-provider-container">{ConfigurationDataForm}</div>;
  }
}
