// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Alert, Tabs, Tab } from 'react-bootstrap';
import PropTypes from 'prop-types';
import { YBFormInput, YBFormDatePicker, YBFormDropZone } from '../../../common/forms/fields';
import { getPromiseState } from '../../../../utils/PromiseUtils';
import { YBModalForm } from '../../../common/forms';
import { isDefinedNotNull, isNonEmptyObject } from '../../../../utils/ObjectUtils';
import { Field } from 'formik';

import MomentLocaleUtils, { formatDate, parseDate } from 'react-day-picker/moment';

const initialValues = {
  certName: '',
  certExpiry: null,
  certContent: null,
  keyContent: null,
  rootCACert: '',
  nodeCertPath: '',
  nodeCertPrivate: '',
  clientCert: '',
  clientCertPrivate: '',
};

// react-day-picker lib requires this to be class component
class DatePickerInput extends Component {
  render() {
    return (
      <div className="date-picker-input" onClick={this.props.onClick}>
        <input {...this.props} />
        <i className="fa fa-calendar" />
      </div>
    );
  }
}

export default class AddCertificateForm extends Component {
  static propTypes = {
    backupInfo: PropTypes.object
  };

  state = {
    tab: 'selfSigned'
  }

  readUploadedFileAsText = (inputFile, isRequired) => {
    const fileReader = new FileReader();
    return new Promise((resolve, reject) => {
      fileReader.onloadend = () => {
        resolve(fileReader.result);
      };
      // Parse the file back to JSON, since the API controller endpoint doesn't support file upload
      if (isDefinedNotNull(inputFile)) {
        fileReader.readAsText(inputFile);
      }
      if (!isRequired && !isDefinedNotNull(inputFile)) {
        resolve(null);
      }
    });
  };

  addCertificate = (vals, setSubmitting) => {
    const self = this;
    const certificateFile = vals.certContent;
    if (this.state.tab === 'selfSigned') {
      const keyFile = vals.keyContent;
      const formValues = {
        label: vals.certName,
        certStart: Date.now(),
        certExpiry: vals.certExpiry.valueOf(),
        certType: 'SelfSigned'
      };
      const fileArray = [
        this.readUploadedFileAsText(certificateFile, false),
        this.readUploadedFileAsText(keyFile, false)
      ];

      // Catch all onload events for configs
      Promise.all(fileArray).then(
        (files) => {
          formValues.certContent = files[0];
          formValues.keyContent = files[1];
          self.props.addCertificate(formValues, setSubmitting);
        },
        (reason) => {
          console.warn('File Upload gone wrong', reason);
          setSubmitting(false);
        }
      );
    } else if (this.state.tab === 'caSigned') {
      const formValues = {
        label: vals.certName,
        certStart: Date.now(),
        certExpiry: vals.certExpiry.valueOf(),
        certType: 'CustomCertHostPath',
        customCertInfo: {
          nodeCertPath: vals.nodeCertPath,
          nodeKeyPath: vals.nodeCertPrivate,
          rootCertPath: vals.rootCACert,
          clientCert: vals.clientCert,
          clientCertPrivate: vals.clientCertPrivate
        }
      };

      this.readUploadedFileAsText(certificateFile, false).then(content => {
        formValues.certContent = content;
        self.props.addCertificate(formValues, setSubmitting);
      }).catch((err) => {
        console.warn(`File Upload gone wrong. ${err}`);
        setSubmitting(false);
      });
    }
  };

  validateForm = (values) => {
    const errors = {};
    if (!values.certName) {
      errors.certName = 'Certificate name is required';
    }
    if (!values.certExpiry) {
      errors.certExpiry = 'Expiration date is required';
    } else {
      const timestamp = Date.parse(values.certExpiry);
      if (isNaN(timestamp) || timestamp < Date.now()) {
        errors.certExpiry = 'Set a valid expiration date';
      }
    }
    if (!values.certContent) {
      errors.certContent = 'Certificate file is required';
    }
    if (this.state.tab === 'selfSigned') {
      if (!values.keyContent) {
        errors.keyContent = 'Key file is required';
      }
    } else if (this.state.tab === 'caSigned') {
      if (!values.rootCACert) {
        errors.rootCACert = 'Root CA certificate is required';
      }
      if (!values.nodeCertPath) {
        errors.nodeCertPath = 'Database node certificate path is required';
      }
      if (!values.nodeCertPrivate) {
        errors.nodeCertPrivate = 'Database node certificate private key is required';
      }
    }

    return errors;
  }

  onHide = () => {
    this.props.onHide();
    this.props.addCertificateReset();
  };

  tabSelect = (newTabKey, formikProps) => {
    const { setFieldTouched, setFieldValue, setErrors, errors, values } = formikProps;
    const newErrors = { ...errors };
    if (this.state.tab !== newTabKey && newTabKey === 'selfSigned') {
      setFieldValue('rootCACert', '');
      setFieldValue('nodeCertPath', '');
      setFieldValue('nodeCertPrivate', '');
      setFieldValue('clientCert', '');
      setFieldValue('clientCertPrivate', '', false);
      if (values.certExpiry instanceof Date) {
        setFieldValue('certExpiry',
          new Date(values.certExpiry.toLocaleDateString('default', {
            month: 'long',
            day: 'numeric',
            year: 'numeric'
          })), false);
      }
      delete newErrors.rootCACert;
      delete newErrors.nodeCertPath;
      delete newErrors.nodeCertPrivate;
    } else if (this.state.tab !== newTabKey && newTabKey === 'caSigned') {
      setFieldValue('keyContent', null, false);
      delete newErrors.keyContent;
      if (values.certExpiry instanceof Date) {
        setFieldValue('certExpiry',
          new Date(values.certExpiry.toLocaleDateString('default', {
            month: 'long',
            day: 'numeric',
            year: 'numeric'
          })), false);
      }
    }
    setFieldTouched('keyContent', false);
    setFieldTouched('rootCACert', false);
    setFieldTouched('nodeCertPath', false);
    setFieldTouched('nodeCertPrivate', false);
    setFieldTouched('clientCert', false);
    setFieldTouched('clientCertPrivate', false);
    setErrors(newErrors);
    this.setState({tab: newTabKey});
  }

  render() {
    const {
      customer: { addCertificate }
    } = this.props;

    return (
      <div className="add-cert-modal">
        <YBModalForm
          title={'Add Certificate'}
          className={getPromiseState(addCertificate).isError() ? 'modal-shake' : ''}
          visible={this.props.visible}
          onHide={this.onHide}
          showCancelButton={true}
          submitLabel="Add"
          cancelLabel="Cancel"
          onFormSubmit={(values, { setSubmitting }) => {
            setSubmitting(true);
            const payload = {
              ...values,
              label: values.certName.trim()
            };
            this.addCertificate(payload, setSubmitting);
          }}
          initialValues={initialValues}
          validate={this.validateForm}
          render={(props) => {
            return (
              <Tabs
                id="add-cert-tabs"
                activeKey={this.state.tab}
                onSelect={(k) => this.tabSelect(k, props)}
              >
                <Tab eventKey="selfSigned" title="Self Signed">
                  <Field name="certName" component={YBFormInput} type="text" label="Certificate Name" required />
                  <Field
                    name="certExpiry"
                    component={YBFormDatePicker}
                    label="Expiration Date"
                    formatDate={formatDate}
                    parseDate={parseDate}
                    format="LL"
                    placeholder="Select Date"
                    dayPickerProps={{
                      localeUtils: MomentLocaleUtils,
                      disabledDays: {
                        before: new Date()
                      }
                    }}
                    required
                    onDayChange={(val) => props.setFieldValue('certExpiry', val)}
                    pickerComponent={DatePickerInput}
                  />
                  <Field
                    name="certContent"
                    component={YBFormDropZone}
                    className="upload-file-button"
                    title="Upload Root Certificate"
                    required
                  />
                  <Field
                    name="keyContent"
                    component={YBFormDropZone}
                    className="upload-file-button"
                    title="Upload Key"
                    required
                  />
                  {getPromiseState(addCertificate).isError() && isNonEmptyObject(addCertificate.error) && (
                    <Alert bsStyle="danger" variant="danger">
                      Certificate adding has been failed:
                      <br />
                      {JSON.stringify(addCertificate.error)}
                    </Alert>
                  )}
                </Tab>
                <Tab eventKey="caSigned" title="CA Signed">
                  <Field name="certName" component={YBFormInput} type="text" label="Certificate Name" required />
                  <Field
                    name="certExpiry"
                    component={YBFormDatePicker}
                    label="Root Certificate Expiration Date"
                    formatDate={formatDate}
                    parseDate={parseDate}
                    format="LL"
                    placeholder="Select Date"
                    dayPickerProps={{
                      localeUtils: MomentLocaleUtils,
                      disabledDays: {
                        before: new Date()
                      }
                    }}
                    required
                    onDayChange={(val) => props.setFieldValue('certExpiry', val)}
                    pickerComponent={DatePickerInput}
                  />
                  <Field
                    name="certContent"
                    component={YBFormDropZone}
                    className="upload-file-button"
                    title="Upload Root Certificate"
                    required
                  />
                  <Field
                    name="rootCACert"
                    component={YBFormInput}
                    label="Root CA Certificate"
                    placeholder="/opt/yugabyte/keys/cert1/ca.crt"
                    required
                  />
                  <Field
                    name="nodeCertPath"
                    component={YBFormInput}
                    label="Database Node Certificate Path"
                    placeholder="/opt/yugabyte/keys/cert1/node.crt"
                    required
                  />
                  <Field
                    name="nodeCertPrivate"
                    component={YBFormInput}
                    label="Database Node Certificate Private Key"
                    placeholder="/opt/yugabyte/keys/cert1/node.key"
                    required
                  />
                  <Field
                    name="clientCert"
                    component={YBFormInput}
                    label="Client Certificate"
                    placeholder="/opt/yugabyte/yugaware/data/cert1/client.crt"
                  />
                  <Field name="clientCertPrivate"
                    component={YBFormInput}
                    label="Client Certificate Private Key"
                    placeholder="/opt/yugabyte/yugaware/data/cert1/client.key"
                  />
                  {getPromiseState(addCertificate).isError() && isNonEmptyObject(addCertificate.error) && (
                    <Alert bsStyle="danger" variant="danger">
                      Certificate adding has been failed:
                      <br />
                      {JSON.stringify(addCertificate.error)}
                    </Alert>
                  )}
                </Tab>
              </Tabs>
            );
          }}
        />
      </div>
    );
  }
}
