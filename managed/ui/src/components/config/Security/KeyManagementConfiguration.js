// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import { Row, Col } from 'react-bootstrap';
import { Field, Formik } from 'formik';
import {
  YBFormInput,
  YBButton,
  YBFormSelect,
  YBCheckBox,
  YBFormDropZone
} from '../../common/forms/fields';
import { YBLoadingCircleIcon } from '../../common/indicators';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { ListKeyManagementConfigurations } from './ListKeyManagementConfigurations';
import * as Yup from 'yup';

import { regionsData } from '../PublicCloud/views/providerRegionsData';
import { readUploadedFile } from '../../../utils/UniverseUtils';
import { change } from 'redux-form';

// TODO: (Daniel) - Replace this hard-coding with an API that returns
//  a list of supported KMS Configurations
const kmsConfigTypes = [
  { value: 'SMARTKEY', label: 'Equinix SmartKey' },
  { value: 'AWS', label: 'AWS KMS' }
];

const awsRegionList = regionsData.map((region, index) => {
  return {
    value: region.destVpcRegion,
    label: region.destVpcRegion
  };
});

class KeyManagementConfiguration extends Component {
  state = {
    listView: false,
    enabledIAMProfile: false,
    useCmkPolicy: false
  };

  updateFormField = (field, value) => {
    this.props.dispatch(change('kmsProviderConfigForm', field, value));
  };

  componentDidMount() {
    this.props.fetchKMSConfigList().then((response) => {
      if (response.payload.data.length) {
        this.setState({ listView: true });
      }
    });
  }

  submitKMSForm = (values) => {
    const { fetchKMSConfigList, setKMSConfig } = this.props;
    const { kmsProvider } = values;
    if (kmsProvider) {
      const data = {};
      data['name'] = values.name;
      switch (kmsProvider.value) {
        case 'AWS':
          data['AWS_REGION'] = values.region.value;
          if (!this.state.enabledIAMProfile) {
            data['AWS_ACCESS_KEY_ID'] = values.accessKeyId;
            data['AWS_SECRET_ACCESS_KEY'] = values.secretKeyId;
          }
          if (values.cmkPolicyContent) {
            readUploadedFile(values.cmkPolicyContent).then((text) => {
              data['cmk_policy'] = text;
              setKMSConfig(kmsProvider.value, data).then(() => {
                fetchKMSConfigList();
                this.setState({ listView: true });
              });
            });
            return;
          } else if (values.cmkId) {
            data['cmk_id'] = values.cmkId;
          }
          break;
        default:
        case 'SMARTKEY':
          data['base_url'] = values.apiUrl || 'api.amer.smartkey.io';
          data['api_key'] = values.apiKey;
          break;
      }
      setKMSConfig(kmsProvider.value, data).then(() => {
        fetchKMSConfigList();
        this.setState({ listView: true });
      });
    }
  };

  getSmartKeyForm = () => {
    return (
      <Fragment>
        <Row className="config-provider-row" key={'url-field'}>
          <Col lg={3}>
            <div className="form-item-custom-label">API Url</div>
          </Col>
          <Col lg={7}>
            <Field
              name={'apiUrl'}
              component={YBFormInput}
              placeholder={'api.amer.smartkey.io'}
              className={'kube-provider-input-field'}
            />
          </Col>
        </Row>
        <Row className="config-provider-row" key={'private-key-field'}>
          <Col lg={3}>
            <div className="form-item-custom-label">Secret API Key</div>
          </Col>
          <Col lg={7}>
            <Field
              name={'apiKey'}
              component={YBFormInput}
              className={'kube-provider-input-field'}
            />
          </Col>
        </Row>
      </Fragment>
    );
  };

  getAWSForm = () => {
    return (
      <Fragment>
        <Row className="config-provider-row" key={'iam-enable-field'}>
          <Col lg={3}>
            <div className="form-item-custom-label">Use IAM Profile</div>
          </Col>
          <Col lg={7}>
            <Field
              name={'enableIAMProfile'}
              component={YBCheckBox}
              input={{
                onChange: () => this.setState({ enabledIAMProfile: !this.state.enabledIAMProfile })
              }}
              className={'kube-provider-input-field'}
            />
          </Col>
        </Row>
        <Row className="config-provider-row" key={'access-key-field'}>
          <Col lg={3}>
            <div className="form-item-custom-label">Access Key Id</div>
          </Col>
          <Col lg={7}>
            <Field
              name={'accessKeyId'}
              component={YBFormInput}
              disabled={this.state.enabledIAMProfile}
              className={'kube-provider-input-field'}
            />
          </Col>
        </Row>
        <Row className="config-provider-row" key={'secret-key-field'}>
          <Col lg={3}>
            <div className="form-item-custom-label">Secret Key Id</div>
          </Col>
          <Col lg={7}>
            <Field
              name="secretKeyId"
              component={YBFormInput}
              disabled={this.state.enabledIAMProfile}
              className={'kube-provider-input-field'}
            />
          </Col>
        </Row>
        <Row className="config-provider-row" key={'region-field'}>
          <Col lg={3}>
            <div className="form-item-custom-label">Region</div>
          </Col>
          <Col lg={7}>
            <Field
              name="region"
              component={YBFormSelect}
              options={awsRegionList}
              className={'kube-provider-input-field'}
            />
          </Col>
        </Row>
        <Row className="cmk-id-row" key={'cmk-id-field'}>
          <Col lg={3}>
            <div className="form-item-custom-label">Customer Master Key ID</div>
          </Col>
          <Col lg={7}>
            <Field
              name={'cmkId'}
              component={YBFormInput}
              placeholder={'CMK ID'}
              className={'kube-provider-input-field'}
            />
          </Col>
        </Row>
        <Row>
          <div className={'bottom-form-field'}>
            <Field
              component={YBFormDropZone}
              name={'cmkPolicyContent'}
              title={'Upload CMK Policy'}
              className="upload-file-button"
            />
          </div>
        </Row>
      </Fragment>
    );
  };

  displayFormContent = (provider) => {
    if (!provider) {
      return this.getSmartKeyForm();
    }
    switch (provider.value) {
      case 'SMARTKEY':
        return this.getSmartKeyForm();
      case 'AWS':
        return this.getAWSForm();
      default:
        return this.getSmartKeyForm();
    }
  };

  openCreateConfigForm = () => {
    this.setState({ listView: false });
  };

  deleteAuthConfig = (configUUID) => {
    const { configList, deleteKMSConfig, fetchKMSConfigList } = this.props;
    deleteKMSConfig(configUUID);
    if (configList.data.length <= 1) {
      this.setState({ listView: false });
    } else {
      fetchKMSConfigList();
    }
  };

  /**
   * Shows list view on click of cancel button by turning the listView flag ON.
   */
  showListView = () => {
    this.setState({ listView: true });
  }

  render() {
    const { configList } = this.props;
    const { listView, enabledIAMProfile } = this.state;
    if (getPromiseState(configList).isInit() || getPromiseState(configList).isLoading()) {
      return <YBLoadingCircleIcon />;
    }
    if (listView) {
      return (
        <ListKeyManagementConfigurations
          configs={configList}
          onCreate={this.openCreateConfigForm}
          onDelete={this.deleteAuthConfig}
        />
      );
    }

    const validationSchema = Yup.object().shape({
      apiUrl: Yup.string(),

      apiKey: Yup.mixed().when('kmsProvider', {
        is: (provider) => provider.value === 'SMARTKEY',
        then: Yup.mixed().required('API key is Required')
      }),

      accessKeyId: Yup.string().when('kmsProvider', {
        is: (provider) => provider.value === 'AWS' && !enabledIAMProfile,
        then: Yup.string().required('Access Key ID is Required')
      }),

      secretKeyId: Yup.string().when('kmsProvider', {
        is: (provider) => provider.value === 'AWS' && !enabledIAMProfile,
        then: Yup.string().required('Secret Key ID is Required')
      }),
      region: Yup.mixed().when('kmsProvider', {
        is: (provider) => provider.value === 'AWS',
        then: Yup.mixed().required('AWS Region is Required')
      }),
      cmkPolicyContent: Yup.string(),

      name: Yup.string().when('kmsProvider', {
        is: (provider) => provider.value !== null,
        then: Yup.string().required('Name is Required')
      }),

      cmkId: Yup.string()
    });

    return (
      <div className="provider-config-container">
        <Formik
          validationSchema={validationSchema}
          onSubmit={(values) => {
            this.submitKMSForm(values);
          }}
        >
          {(props) => (
            <form onSubmit={props.handleSubmit}>
              <Row>
                <Col lg={8}>
                  <Row className="config-name-row" key={'name-field'}>
                    <Col lg={3}>
                      <div className="form-item-custom-label">Configuration Name</div>
                    </Col>
                    <Col lg={7}>
                      <Field
                        name={'name'}
                        component={YBFormInput}
                        placeholder={'Configuration Name'}
                        className={'kube-provider-input-field'}
                      />
                    </Col>
                  </Row>
                  <Row className="config-provider-row" key={'provider-field'}>
                    <Col lg={3}>
                      <div className="form-item-custom-label">KMS Provider</div>
                    </Col>
                    <Col lg={7}>
                      <Field
                        name="kmsProvider"
                        placeholder="Provider name"
                        component={YBFormSelect}
                        options={kmsConfigTypes}
                        className={'kube-provider-input-field'}
                      />
                    </Col>
                  </Row>
                  {this.displayFormContent(props.values.kmsProvider)}
                </Col>
              </Row>
              <div className="form-action-button-container">
                <YBButton
                  btnText="Save"
                  btnClass="btn btn-orange"
                  btnType="submit"
                />
                <YBButton
                  btnText="Cancel"
                  btnClass="btn btn-orange"
                  onClick={this.showListView}
                />
              </div>
            </form>
          )}
        </Formik>
      </div>
    );
  }
}
export default KeyManagementConfiguration;
