// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import {
  YBFormInput,
  YBButton,
  YBToggle,
  YBControlledSelectWithLabel
} from '../common/forms/fields';
import { Formik, Form, Field } from 'formik';
import { isDisabled, showOrRedirect } from '../../utils/LayoutUtils';
import * as Yup from 'yup';
import _ from 'lodash';
import { isNonEmptyObject, isNonEmptyArray } from '../../utils/ObjectUtils';
import { getPromiseState } from '../../utils/PromiseUtils';

// TODO set predefined defaults another way not to share defaults this way
const CHECK_INTERVAL_MS = 300000;
const STATUS_UPDATE_INTERVAL_MS = 43200000;
const DEFAULT_SMTP_PORT = 587;

const validationSchema = Yup.object().shape({
  alertingData: Yup.object({
    sendAlertsToYb: Yup.boolean().default(false).nullable(),
    alertingEmail: Yup.string().nullable(), // This field can be one or more emails separated by commas
    checkIntervalMs: Yup.number().typeError('Must specify a number'),
    statusUpdateIntervalMs: Yup.number().typeError('Must specify a number'),
    reportOnlyErrors: Yup.boolean().default(false).nullable(),
    reportBackupFailures: Yup.boolean().default(false).nullable()
  }),
  customSmtp: Yup.boolean(),
  smtpData: Yup.object().when('customSmtp', {
    is: true,
    then: Yup.object({
      smtpServer: Yup.string().required('Must specify an SMTP server address'),
      smtpPort: Yup.number().typeError('Must specify an SMTP server port number'),
      emailFrom: Yup.string().email('Must be an email'),
      smtpUsername: Yup.string(),
      smtpPassword: Yup.string(),
      useSSL: Yup.boolean(),
      useTLS: Yup.boolean()
    })
  }),
  callhomeLevel: Yup.string()
});

const callhomeOptions = [
  <option value="NONE" key={0}>
    None
  </option>,
  <option value="LOW" key={1}>
    Low
  </option>,
  <option value="MEDIUM" key={2}>
    Medium
  </option>,
  <option value="HIGH" key={3}>
    High
  </option>
];

export default class AlertProfileForm extends Component {
  constructor(props) {
    super(props);
    this.state = {
      statusUpdated: false
    };
  }

  componentDidUpdate() {
    const { customerProfile, handleProfileUpdate } = this.props;
    const { statusUpdated } = this.state;
    if (
      statusUpdated &&
      (getPromiseState(customerProfile).isSuccess() || getPromiseState(customerProfile).isError())
    ) {
      handleProfileUpdate(customerProfile.data);
      this.setState({ statusUpdated: false });
    }
  }

  render() {
    const { customer = {}, users = [], updateCustomerDetails } = this.props;

    showOrRedirect(customer.data.features, 'main.profile');

    // Filter users for userUUID set during login
    const loginUserId = localStorage.getItem('userId');
    const getCurrentUser = isNonEmptyArray(users)
      ? users.filter((u) => u.uuid === loginUserId)
      : [];
    const initialValues = {
      name: customer.data.name || '',
      email: (getCurrentUser.length && getCurrentUser[0].email) || '',
      code: customer.data.code || '',

      alertingData: {
        alertingEmail: customer.data.alertingData
          ? customer.data.alertingData.alertingEmail || ''
          : '',
        checkIntervalMs: getPromiseState(customer).isSuccess()
          ? isNonEmptyObject(customer.data.alertingData)
            ? customer.data.alertingData.checkIntervalMs
            : CHECK_INTERVAL_MS
          : '',
        statusUpdateIntervalMs: getPromiseState(customer).isSuccess()
          ? isNonEmptyObject(customer.data.alertingData)
            ? customer.data.alertingData.statusUpdateIntervalMs
            : STATUS_UPDATE_INTERVAL_MS
          : '',
        sendAlertsToYb: customer.data.alertingData && customer.data.alertingData.sendAlertsToYb,
        reportOnlyErrors: customer.data.alertingData && customer.data.alertingData.reportOnlyErrors,
        reportBackupFailures: customer.data.alertingData && customer.data.alertingData.reportBackupFailures
      },
      customSmtp: isNonEmptyObject(_.get(customer, 'data.smtpData', {})),
      smtpData: {
        smtpServer: _.get(customer, 'data.smtpData.smtpServer', ''),
        smtpPort: _.get(customer, 'data.smtpData.smtpPort', DEFAULT_SMTP_PORT),
        emailFrom: _.get(customer, 'data.smtpData.emailFrom', ''),
        // ensure username and password are always strings, even if API returns "null" as a value
        smtpUsername: _.get(customer, 'data.smtpData.smtpUsername') || '',
        smtpPassword: _.get(customer, 'data.smtpData.smtpPassword') || '',
        useSSL: _.get(customer, 'data.smtpData.useSSL', false),
        useTLS: _.get(customer, 'data.smtpData.useTLS', false)
      },
      callhomeLevel: customer.data.callhomeLevel || 'NONE'
    };

    return (
      <div className="bottom-bar-padding">
        <Formik
          validationSchema={validationSchema}
          initialValues={initialValues}
          enableReinitialize
          onSubmit={(values, { setSubmitting, resetForm }) => {
            const data = _.omit(values, 'customSmtp'); // don't submit internal helper field
            if (values.customSmtp) {
              // due to smtp specifics have to remove smtpUsername/smtpPassword props from payload when they are empty
              if (!data.smtpData.smtpUsername)
                data.smtpData = _.omit(data.smtpData, 'smtpUsername');
              if (!data.smtpData.smtpPassword)
                data.smtpData = _.omit(data.smtpData, 'smtpPassword');
            } else {
              data.smtpData = null; // this will revert smtp settings to default presets
            }

            updateCustomerDetails(data);
            this.setState({ statusUpdated: true });
            setSubmitting(false);

            // default form to new values to avoid unwanted validation of smtp fields when they are hidden
            resetForm(values);
          }}
        >
          {({ values, handleChange, handleSubmit, isSubmitting }) => (
            <Form name="EditCustomerProfile" onSubmit={handleSubmit}>
              <Row>
                <Col md={6} sm={12}>
                  <h3>Alerting controls</h3>
                  <Field
                    name="alertingData.alertingEmail"
                    type="text"
                    component={YBFormInput}
                    label="Alert emails"
                    placeholder="Emails to forward alerts to"
                  />
                  <Field name="alertingData.sendAlertsToYb">
                    {({ field }) => (
                      <YBToggle
                        onToggle={handleChange}
                        name="alertingData.sendAlertsToYb"
                        input={{
                          value: field.value,
                          onChange: field.onChange
                        }}
                        label="Send alert emails to YugaByte team"
                        subLabel="Whether or not to send alerting emails to the YugaByte team."
                      />
                    )}
                  </Field>
                  <Field
                    name="callhomeLevel"
                    component={YBControlledSelectWithLabel}
                    label="Callhome Level"
                    input={{ name: 'callhomeLevel' }}
                    onInputChanged={handleChange}
                    selectVal={values.callhomeLevel}
                    options={callhomeOptions}
                  />
                  <Field
                    name="alertingData.checkIntervalMs"
                    type="text"
                    component={YBFormInput}
                    label="Health check interval"
                    placeholder="Milliseconds to check universe status"
                  />
                  <Field
                    name="alertingData.statusUpdateIntervalMs"
                    type="text"
                    component={YBFormInput}
                    label="Report email interval"
                    placeholder="Milliseconds to send a status report email"
                  />
                  <Field name="alertingData.reportOnlyErrors">
                    {({ field }) => (
                      <YBToggle
                        onToggle={handleChange}
                        name="alertingData.reportOnlyErrors"
                        input={{
                          value: field.value,
                          onChange: field.onChange
                        }}
                        label="Only include errors in alert emails"
                        subLabel="Whether or not to include errors in alert emails."
                      />
                    )}
                  </Field>
                  <Field name="alertingData.reportBackupFailures">
                    {({ field }) => (
                      <YBToggle
                        onToggle={handleChange}
                        name="alertingData.reportBackupFailures"
                        input={{
                          value: field.value,
                          onChange: field.onChange
                        }}
                        label="Send backup failure notification"
                        subLabel="Whether or not to send an email if a backup task fails."
                      />
                    )}
                  </Field>
                </Col>
              </Row>
              <Row>
                <br />
                <Col md={6} sm={12}>
                  <Field name="customSmtp">
                    {({ field }) => (
                      <YBToggle
                        onToggle={handleChange}
                        name="customSmtp"
                        input={{
                          value: field.value,
                          onChange: field.onChange
                        }}
                        label={<h3>Custom SMTP Configuration</h3>}
                        subLabel="Whether or not to use custom SMTP Configuration."
                      />
                    )}
                  </Field>
                  <div hidden={!values.customSmtp}>
                    <Field
                      name="smtpData.smtpServer"
                      type="text"
                      component={YBFormInput}
                      label="Server"
                      placeholder="SMTP server address"
                    />
                    <Field
                      name="smtpData.smtpPort"
                      type="text"
                      component={YBFormInput}
                      label="Port"
                      placeholder="SMTP server port"
                    />
                    <Field
                      name="smtpData.emailFrom"
                      type="text"
                      component={YBFormInput}
                      label="Email From"
                      placeholder="Send outgoing emails from"
                    />
                    <Field
                      name="smtpData.smtpUsername"
                      type="text"
                      component={YBFormInput}
                      label="Username"
                      placeholder="SMTP server username"
                    />
                    <Field
                      name="smtpData.smtpPassword"
                      type="password"
                      autoComplete="new-password"
                      component={YBFormInput}
                      label="Password"
                      placeholder="SMTP server password"
                    />
                    <Field name="smtpData.useSSL">
                      {({ field }) => (
                        <YBToggle
                          onToggle={handleChange}
                          name="smtpData.useSSL"
                          input={{
                            value: field.value,
                            onChange: field.onChange
                          }}
                          label="SSL"
                          subLabel="Whether or not to use SSL."
                        />
                      )}
                    </Field>
                    <Field name="smtpData.useTLS">
                      {({ field }) => (
                        <YBToggle
                          onToggle={handleChange}
                          name="smtpData.useTLS"
                          input={{
                            value: field.value,
                            onChange: field.onChange
                          }}
                          label="TLS"
                          subLabel="Whether or not to use TLS."
                        />
                      )}
                    </Field>
                  </div>
                </Col>
              </Row>
              <div className="form-action-button-container">
                <Col sm={12}>
                  <YBButton
                    btnText="Save"
                    btnType="submit"
                    disabled={isSubmitting || isDisabled(customer.data.features, 'universe.create')}
                    btnClass="btn btn-orange pull-right"
                  />
                </Col>
              </div>
            </Form>
          )}
        </Formik>
      </div>
    );
  }
}
