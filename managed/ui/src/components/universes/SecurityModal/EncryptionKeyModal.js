// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { Field, Formik } from 'formik';
import * as Yup from 'yup';
import 'react-bootstrap-multiselect/css/bootstrap-multiselect.css';
import { YBModal, YBFormToggle, YBFormSelect } from '../../common/forms/fields';
import { isNonEmptyObject } from '../../../utils/ObjectUtils';

export default class EncryptionKeyModal extends Component {
  componentDidMount() {
    const { configList, fetchKMSConfigList } = this.props;
    if (!configList.data.length) {
      fetchKMSConfigList();
    }
  }

  handleSubmitForm = (values) => {
    const {
      currentUniverse: {
        data: { universeUUID, universeDetails }
      },
      setEncryptionKey,
      handleSubmitKey
    } = this.props;
    const encryptionAtRestEnabled =
      universeDetails.encryptionAtRestConfig &&
      universeDetails.encryptionAtRestConfig.encryptionAtRestEnabled;

    // When the both the encryption enabled and rotate key values didn't change
    // we don't submit the form.
    if (encryptionAtRestEnabled === values.enableEncryptionAtRest && !values.rotateKey) {
      return false;
    }
    // When the form is submitted without changing the KMS provider select,
    // we would have the value as string otherwise it would be an object.
    const kmsConfigUUID = isNonEmptyObject(values.selectKMSProvider)
      ? values.selectKMSProvider.value
      : values.selectKMSProvider;

    const data = {
      key_op: values.enableEncryptionAtRest ? 'ENABLE' : 'DISABLE',
      kmsConfigUUID: kmsConfigUUID
    };

    handleSubmitKey(setEncryptionKey(universeUUID, data));
  };

  render() {
    const { modalVisible, onHide, configList, currentUniverse } = this.props;
    const {
      data: { universeDetails }
    } = currentUniverse;
    const encryptionAtRestConfig = universeDetails.encryptionAtRestConfig;
    const encryptionAtRestEnabled =
      encryptionAtRestConfig && encryptionAtRestConfig.encryptionAtRestEnabled;
    const labelText = currentUniverse.data.name
      ? `Enable Encryption-at-Rest for ${this.props.name}?`
      : 'Enable Encryption-at-Rest?';
    const kmsOptions = configList.data.map((config) => ({
      value: config.metadata.configUUID,
      label: config.metadata.provider + ' - ' + config.metadata.name
    }));

    const initialValues = {
      enableEncryptionAtRest: encryptionAtRestEnabled,
      selectKMSProvider: null,
      awsCmkPolicy: null,
      rotateKey: false,
      key_type: 'DATA_KEY'
    };

    const validationSchema = Yup.object().shape({
      enableEncryptionAtRest: Yup.boolean(),
      selectKMSProvider: Yup.mixed().when('enableEncryptionAtRest', {
        is: true,
        then: Yup.mixed().required('KMS Provider is required')
      })
    });

    if (isNonEmptyObject(encryptionAtRestConfig) && encryptionAtRestConfig.kmsConfigUUID) {
      initialValues.selectKMSProvider = encryptionAtRestConfig.kmsConfigUUID;
    }

    return (
      <Formik
        initialValues={initialValues}
        validationSchema={validationSchema}
        onSubmit={(values) => {
          this.handleSubmitForm(values);
        }}
      >
        {(props) => (
          <YBModal
            visible={modalVisible}
            formName={'EncryptionForm'}
            onHide={onHide}
            onFormSubmit={props.handleSubmit}
            submitLabel={'Submit'}
            cancelLabel={'Close'}
            showCancelButton={true}
            title="Manage Encryption at-Rest"
          >
            <div className="manage-key-container">
              <Row>
                <Col lg={7}>
                  <div className="form-item-custom-label">{labelText}</div>
                </Col>
                <Col lg={3}>
                  <Field name="enableEncryptionAtRest" component={YBFormToggle} />
                </Col>
              </Row>
              {props.values.enableEncryptionAtRest && (
                <Row className="config-provider-row">
                  <Col lg={4}>
                    <div className="form-item-custom-label">Key Management Service Config</div>
                  </Col>
                  <Col lg={7}>
                    <Field name="selectKMSProvider" component={YBFormSelect} options={kmsOptions} />
                  </Col>
                </Row>
              )}
              {encryptionAtRestEnabled && (
                <Row>
                  <Col lg={7}>
                    <div className="form-item-custom-label">{'Rotate Key?'}</div>
                  </Col>
                  <Col lg={3}>
                    <Field name="rotateKey" component={YBFormToggle} />
                  </Col>
                </Row>
              )}
            </div>
          </YBModal>
        )}
      </Formik>
    );
  }
}
