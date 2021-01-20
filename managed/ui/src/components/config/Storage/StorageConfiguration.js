// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Tab, Row, Col } from 'react-bootstrap';
import _ from 'lodash';
import { YBTabsPanel } from '../../panels';
import { YBButton, YBTextInputWithLabel } from '../../common/forms/fields';
import { withRouter } from 'react-router';
import { Field, SubmissionError } from 'redux-form';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { YBLoading } from '../../common/indicators';
import { YBConfirmModal } from '../../modals';
import AwsStorageConfiguration from './AwsStorageConfiguration';
import YBInfoTip from '../../common/descriptors/YBInfoTip';

import awss3Logo from './images/aws-s3.png';
import azureLogo from './images/azure_logo.svg';
import {
  isNonEmptyObject,
  isEmptyObject,
  isDefinedNotNull
} from '../../../utils/ObjectUtils';

const storageConfigTypes = {
  NFS: {
    title: 'NFS Storage',
    fields: [
      {
        id: 'BACKUP_LOCATION',
        label: 'NFS Storage Path',
        placeHolder: 'NFS Storage Path'
      }
    ]
  },
  GCS: {
    title: 'GCS Storage',
    fields: [
      {
        id: 'BACKUP_LOCATION',
        label: 'GCS Bucket',
        placeHolder: 'GCS Bucket'
      },
      {
        id: 'GCS_CREDENTIALS_JSON',
        label: 'GCS Credentials',
        placeHolder: 'GCS Credentials JSON'
      }
    ]
  },
  AZ: {
    title: 'Azure Storage',
    fields: [
      {
        id: 'BACKUP_LOCATION',
        label: 'Container URL',
        placeHolder: 'Container URL'
      },
      {
        id: 'AZURE_STORAGE_SAS_TOKEN',
        label: 'SAS Token',
        placeholder: 'SAS Token'
      }
    ]
  }
};

const getTabTitle = (configName) => {
  switch (configName) {
    case 'S3':
      return <img src={awss3Logo} alt="AWS S3" className="aws-logo" />;
    case 'GCS':
      return (
        <h3>
          <i className="fa fa-database"></i>GCS
        </h3>
      );
    case 'AZ':
      return <img src={azureLogo} alt="Azure" className="azure-logo" />;
    default:
      return (
        <h3>
          <i className="fa fa-database"></i>NFS
        </h3>
      );
  }
};

class StorageConfiguration extends Component {
  getConfigByType = (name, customerConfigs) => {
    return customerConfigs.data.find((config) => config.name.toLowerCase() === name);
  };

  wrapFields = (configFields, configName, configControls) => {
    const configNameFormatted = configName.toLowerCase();
    return (
      <Tab
        eventKey={configNameFormatted}
        title={getTabTitle(configName)}
        key={configNameFormatted + '-tab'}
        unmountOnExit={true}
      >
        <Row className="config-section-header" key={configNameFormatted}>
          <Col lg={8}>{configFields}</Col>
          {configControls && <Col lg={4}>{configControls}</Col>}
        </Row>
      </Tab>
    );
  };

  addStorageConfig = (values, action, props) => {
    const type =
      (props.activeTab && props.activeTab.toUpperCase()) || Object.keys(storageConfigTypes)[0];
    Object.keys(values).forEach((key) => {
      if (typeof values[key] === 'string' || values[key] instanceof String)
        values[key] = values[key].trim();
    });
    let dataPayload = { ...values };

    // These conditions will pick only the required JSON keys from the respective tab.
    switch (props.activeTab) {
      case 'nfs':
        dataPayload = _.pick(dataPayload, ['BACKUP_LOCATION']);
        break;

      case 'gcs':
        dataPayload = _.pick(dataPayload, ['BACKUP_LOCATION', 'GCS_CREDENTIALS_JSON']);
        break;

      case 'az':
        dataPayload = _.pick(dataPayload, ['BACKUP_LOCATION', 'AZURE_STORAGE_SAS_TOKEN']);
        break;

      default:
        if (values['IAM_INSTANCE_PROFILE']) {
          dataPayload['IAM_INSTANCE_PROFILE'] = dataPayload['IAM_INSTANCE_PROFILE'].toString();
          dataPayload = _.pick(dataPayload, [
            'BACKUP_LOCATION',
            'AWS_HOST_BASE',
            'IAM_INSTANCE_PROFILE'
          ]);
        } else {
           dataPayload = _.pick(dataPayload, [
            'AWS_ACCESS_KEY_ID',
            'AWS_SECRET_ACCESS_KEY',
            'BACKUP_LOCATION',
            'AWS_HOST_BASE'
          ]);
        }
        break;
    }

    return this.props
      .addCustomerConfig({
        type: 'STORAGE',
        name: type,
        data: dataPayload
      })
      .then((resp) => {
        if (getPromiseState(this.props.addConfig).isSuccess()) {
          // reset form after successful submission due to BACKUP_LOCATION value is shared across all tabs
          this.props.reset();
          this.props.fetchCustomerConfigs();
        } else if (getPromiseState(this.props.addConfig).isError()) {
          // show server-side validation errors under form inputs
          throw new SubmissionError(this.props.addConfig.error);
        }
      });
  };

  deleteStorageConfig = (configUUID) => {
    this.props.deleteCustomerConfig(configUUID)
      .then(() => {
        this.props.reset(); // reset form to initial values
        this.props.fetchCustomerConfigs();
      });
  };

  showDeleteConfirmModal = (configName) => {
    this.props.showDeleteStorageConfig(configName);
  };

  componentDidMount() {
    this.props.fetchCustomerConfigs();
  }

  render() {
    const {
      handleSubmit,
      submitting,
      addConfig: { loading },
      customerConfigs
    } = this.props;
    if (getPromiseState(customerConfigs).isLoading()) {
      return <YBLoading />;
    }

    if (
      getPromiseState(customerConfigs).isSuccess() ||
      getPromiseState(customerConfigs).isEmpty()
    ) {
      const configs = [
        <Tab
          eventKey={'s3'}
          title={getTabTitle('S3')}
          key={'s3-tab'}
          unmountOnExit={true}>
          <AwsStorageConfiguration
            {...this.props}
            deleteStorageConfig={this.deleteStorageConfig}
          />
        </Tab>
      ];
      Object.keys(storageConfigTypes).forEach((configName) => {
        const config = customerConfigs.data.find((config) => config.name === configName);
        if (isDefinedNotNull(config)) {
          const configData = [];
          const storageConfig = storageConfigTypes[config.name];
          if (!isDefinedNotNull(storageConfig)) {
            return;
          }
          const fieldAttrs = storageConfigTypes[config.name]['fields'];
          for (const configItem in config.data) {
            const configAttr = fieldAttrs.find((item) => item.id === configItem);
            if (isDefinedNotNull(configAttr)) {
              configData.push({ name: configAttr.label, data: config.data[configItem] });
            }
          }

          const configFields = [];
          const configTemplate = storageConfigTypes[configName];
          configTemplate.fields.forEach((field) => {
            const value = config.data[field.id];
            configFields.push(
              <Row className="config-provider-row" key={configName + field.id}>
                <Col lg={2}>
                  <div className="form-item-custom-label">{field.label}</div>
                </Col>
                <Col lg={10}>
                  <Field
                    name={field.id}
                    placeHolder={field.placeHolder}
                    input={{ value: value, disabled: isDefinedNotNull(value) }}
                    component={YBTextInputWithLabel}
                  />
                </Col>
              </Row>
            );
          });

          const configControls = (
            <div className="action-bar">
              {config.inUse && (
                <YBInfoTip content={"Storage configuration is in use and cannot be deleted until associated resources are removed."}
                  placement="top"
                >
                  <span className="disable-delete fa-stack fa-2x">
                    <i className="fa fa-trash-o fa-stack-1x"></i>
                    <i className="fa fa-ban fa-stack-2x"></i>
                  </span>
                </YBInfoTip>
              )}
              <YBButton
                btnText={'Delete Configuration'}
                disabled={config.inUse || submitting || loading || isEmptyObject(config)}
                btnClass={'btn btn-default'}
                onClick={
                  isDefinedNotNull(config)
                    ? this.showDeleteConfirmModal.bind(this, config.name)
                    : () => {}
                }
              />
              {isDefinedNotNull(config) && (
                <YBConfirmModal
                  name="delete-storage-config"
                  title={'Confirm Delete'}
                  onConfirm={handleSubmit(this.deleteStorageConfig.bind(this, config.configUUID))}
                  currentModal={'delete' + config.name + 'StorageConfig'}
                  visibleModal={this.props.visibleModal}
                  hideConfirmModal={this.props.hideDeleteStorageConfig}
                >
                  Are you sure you want to delete {config.name} Storage Configuration?
                </YBConfirmModal>
              )}
            </div>
          );

          configs.push(this.wrapFields(configFields, configName, configControls));
        } else {
          const configFields = [];
          const config = storageConfigTypes[configName];
          config.fields.forEach((field) => {
            configFields.push(
              <Row className="config-provider-row" key={configName + field.id}>
                <Col lg={2}>
                  <div className="form-item-custom-label">{field.label}</div>
                </Col>
                <Col lg={10}>
                  <Field
                    name={field.id}
                    placeHolder={field.placeHolder}
                    component={YBTextInputWithLabel}
                  />
                </Col>
              </Row>
            );
          });
          configs.push(this.wrapFields(configFields, configName));
        }
      });

      const activeTab = this.props.activeTab || Object.keys(storageConfigTypes)[0].toLowerCase();
      const config = this.getConfigByType(activeTab, customerConfigs);

      return (
        <div className="provider-config-container">
          <form name="storageConfigForm" onSubmit={handleSubmit(this.addStorageConfig)}>
            <YBTabsPanel
              defaultTab={Object.keys(storageConfigTypes)[0].toLowerCase()}
              activeTab={activeTab}
              id="storage-config-tab-panel"
              className="config-tabs"
              routePrefix="/config/backup/"
            >
              {configs}
            </YBTabsPanel>

            <div className="form-action-button-container">
              <YBButton
                btnText={'Save'}
                btnClass={'btn btn-orange'}
                disabled={submitting || loading || isNonEmptyObject(config)}
                btnType="submit"
              />
            </div>
          </form>
        </div>
      );
    }
    return <YBLoading />;
  }
}

export default withRouter(StorageConfiguration);
