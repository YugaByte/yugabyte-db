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
import { BackupList } from './BackupList';

const storageConfigTypes = {
  NFS: {
    title: 'NFS Storage',
    fields: [
      {
        id: 'NFS_CONFIGURATION_NAME',
        label: 'Configuration Name',
        placeHolder: 'Configuration Name'
      },
      {
        id: 'NFS_BACKUP_LOCATION',
        label: 'NFS Storage Path',
        placeHolder: 'NFS Storage Path'
      }
    ]
  },
  GCS: {
    title: 'GCS Storage',
    fields: [
      {
        id: 'GCS_CONFIGURATION_NAME',
        label: 'Configuration Name',
        placeHolder: 'Configuration Name'
      },
      {
        id: 'GCS_BACKUP_LOCATION',
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
        id: 'AZ_CONFIGURATION_NAME',
        label: 'Configuration Name',
        placeHolder: 'Configuration Name'
      },
      {
        id: 'AZ_BACKUP_LOCATION',
        label: 'Container URL',
        placeHolder: 'Container URL'
      },
      {
        id: 'AZURE_STORAGE_SAS_TOKEN',
        label: 'SAS Token',
        placeHolder: 'SAS Token'
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
  constructor(props) {
    super(props);

    this.state = {
      listview: {
        s3: true,
        nfs: true,
        gcs: true,
        az: true
      },
      iamRoleEnabled: false
    };
  }

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
        {!this.state.listview[configNameFormatted] &&
          <Row className="config-section-header" key={configNameFormatted}>
            <Col lg={8}>{configFields}</Col>
            {configControls && <Col lg={4}>{configControls}</Col>}
          </Row>
        }
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
    let configName = "";

    // These conditions will pick only the required JSON keys from the respective tab.
    switch (props.activeTab) {
      case 'nfs':
        configName = dataPayload['NFS_CONFIGURATION_NAME'];
        dataPayload['BACKUP_LOCATION'] = dataPayload['NFS_BACKUP_LOCATION'];
        dataPayload = _.pick(dataPayload, ['BACKUP_LOCATION']);
        break;

      case 'gcs':
        configName = dataPayload['GCS_CONFIGURATION_NAME'];
        dataPayload['BACKUP_LOCATION'] = dataPayload['GCS_BACKUP_LOCATION'];
        dataPayload = _.pick(dataPayload, [
          'BACKUP_LOCATION',
          'GCS_CREDENTIALS_JSON'
        ]);
        break;

      case 'az':
        configName = dataPayload['AZ_CONFIGURATION_NAME'];
        dataPayload['BACKUP_LOCATION'] = dataPayload['AZ_BACKUP_LOCATION'];
        dataPayload = _.pick(dataPayload, [
          'BACKUP_LOCATION',
          'AZURE_STORAGE_SAS_TOKEN'
        ]);
        break;

      default:
        if (values['IAM_INSTANCE_PROFILE']) {
          configName = dataPayload['AWS_CONFIGURATION_NAME'];
          dataPayload['IAM_INSTANCE_PROFILE'] = dataPayload['IAM_INSTANCE_PROFILE'].toString();
          dataPayload['BACKUP_LOCATION'] = dataPayload['AWS_BACKUP_LOCATION'];
          dataPayload = _.pick(dataPayload, [
            'BACKUP_LOCATION',
            'AWS_HOST_BASE',
            'IAM_INSTANCE_PROFILE'
          ]);
        } else {
          configName = dataPayload['AWS_CONFIGURATION_NAME'];
          dataPayload['BACKUP_LOCATION'] = dataPayload['AWS_BACKUP_LOCATION'];
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
        configName: configName,
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

  // This method will enable the create backup form.
  createBackupConfig = (activeTab) => {
    this.setState({
      listview: {
        ...this.state.listview,
        [activeTab]: false
      }
    });
  };

  // This method will enable the backup list view.
  showListView = (activeTab) => {
    this.setState({
      listview: {
        ...this.state.listview,
        [activeTab]: true
      }
    });
  };

  // This method will disbale the access key and secret key
  // field if IAM role is enabled.
  iamInstanceToggle = (event) => {
    this.setState({ iamRoleEnabled: event.target.checked });
  };

  render() {
    const {
      handleSubmit,
      submitting,
      addConfig: { loading },
      customerConfigs
    } = this.props;
    const activeTab = this.props.activeTab || Object.keys(storageConfigTypes)[0].toLowerCase();
    const config = this.getConfigByType(activeTab, customerConfigs);
    const backupListData = customerConfigs.data.filter((list) => {
      if (activeTab === list.name.toLowerCase()) {
        return list;
      }
    });
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
          unmountOnExit={true}
        >
          {!this.state.listview.s3 &&
            <AwsStorageConfiguration
              {...this.props}
              deleteStorageConfig={this.deleteStorageConfig}
              iamRoleEnabled={this.state.iamRoleEnabled}
              iamInstanceToggle={this.iamInstanceToggle}
            />
          }
        </Tab>
      ];
      Object.keys(storageConfigTypes).forEach((configName) => {
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
      });

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
              {this.state.listview[activeTab] &&
                <BackupList
                  activeTab={activeTab}
                  data={backupListData}
                  onCreateBackup={() => this.createBackupConfig(activeTab)}
                />
              }

              {configs}
            </YBTabsPanel>

            {!this.state.listview[activeTab] &&
              <div className="form-action-button-container">
                <YBButton
                  btnText='Save'
                  btnClass='btn btn-orange'
                  btnType="submit"
                />
                <YBButton
                  btnText='Cancel'
                  btnClass='btn btn-orange'
                  onClick={() => this.showListView(activeTab)}
                />
              </div>
            }
          </form>
        </div>
      );
    }
    return <YBLoading />;
  }
}

export default withRouter(StorageConfiguration);
