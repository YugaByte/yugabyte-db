import { Field } from 'formik';
import React, { useState } from 'react';
import { Col, Row } from 'react-bootstrap';
import { YBModalForm } from '../../common/forms';
import { YBFormInput, YBSelectWithLabel, YBToggle } from '../../common/forms/fields';

export const AddDestinationChannelFrom = (props) => {
  const { visible, onHide, defaultChannel } = props;
  const [channelType, setChannelType] = useState(defaultChannel);
  const [customSMTP, setCustomSMTP] = useState(true);

  // TODO: Add option for pagerDuty oce API is avaialable
  const targetTypeList = [
    <option key={1} value={'email'}>
      {'Email'}
    </option>,
    <option key={2} value={'slack'}>
      {'Slack'}
    </option>
  ];

  /**
   * Hide the modal after setting the custom smtp flag to false.
   */
  const onModalHide = () => {
    setCustomSMTP(true);
    onHide();
  };

  /**
   * Create the payload based on channel type and add the channel.
   * @param {FormValues} values
   */
  const handleAddDestination = (values) => {
    let payload = {
      name: '',
      params: {}
    };

    switch (values.ALERT_TARGET_TYPE) {
      case 'slack':
        payload['name'] = values['slack_name'];
        payload['params']['targetType'] = 'Slack';
        payload['params']['webhookUrl'] = values.webhookURL;
        payload['params']['channel'] = values.name;
        break;
      case 'email':
        // values.smtpData.smtpPort = parseInt(values.smtpData.smtpPort);
        payload['name'] = values['email_name'];
        payload['params']['targetType'] = 'Email';
        payload['params']['recipients'] = values.emailIds.split(',');
        if (!customSMTP) {
          payload['params']['smtpData'] = values.smtpData;
        } else {
          payload['params']['defaultSmtpSettings'] = true;
        }
        break;
      default:
        break;
    }
    props.createAlertChannel(payload).then(() => {
      props.getAlertReceivers().then((receivers) => {
        receivers = receivers.map((receiver) => {
          return {
            value: receiver['uuid'],
            label: receiver['name']
          };
        });
        props.updateDestinationChannel(receivers);
      });
    });
    onModalHide();
  };

  const handleOnToggle = (event) => {
    const name = event.target.name;
    const value = event.target.checked;
    if (name === 'customSmtp') {
      setCustomSMTP(!value);
    }
  };

  const handleChannelTypeChange = (value) => {
    setChannelType(value);
  };

  const getChannelForm = () => {
    switch (channelType) {
      case 'pagerDuty':
        return (
          <>
            <Row>
              <Col lg={12}>
                <Field
                  name="pagerDuty_name"
                  type="text"
                  label="Name"
                  placeholder="Enter channel name"
                  component={YBFormInput}
                />
              </Col>
            </Row>
            <Row>
              <Col lg={12}>
                <Field
                  name="serviceKey"
                  type="text"
                  label="PagerDuty Service Integration Key"
                  placeholder="Enter service integration key"
                  component={YBFormInput}
                />
              </Col>
            </Row>
          </>
        );
      case 'slack':
        return (
          <>
            <Row>
              <Col lg={12}>
                <Field
                  name="slack_name"
                  type="text"
                  placeholder="Enter channel name"
                  label="Name"
                  component={YBFormInput}
                />
              </Col>
            </Row>
            <Row>
              <Col lg={12}>
                <Field
                  name="webhookURL"
                  type="text"
                  label="Slack Webhook URL"
                  placeholder="Enter webhook url"
                  component={YBFormInput}
                />
              </Col>
            </Row>
          </>
        );
      case 'email':
        return (
          <>
            <Row>
              <Col lg={12}>
                <Field
                  name="email_name"
                  type="text"
                  label="Name"
                  placeholder="Enter channel name"
                  component={YBFormInput}
                />
              </Col>
            </Row>
            <Row>
              <Col lg={12}>
                <Field
                  name="emailIds"
                  type="text"
                  label="Emails"
                  placeholder="Enter email addressess"
                  component={YBFormInput}
                />
              </Col>
            </Row>
            <Row>
              <Col lg={12}>
                <Field name="customSmtp">
                  {({ field }) => (
                    <YBToggle
                      onToggle={handleOnToggle}
                      name="customSmtp"
                      input={{
                        value: field.value,
                        onChange: field.onChange
                      }}
                      label="Custom SMTP Configuration"
                      subLabel="Whether or not to use custom SMTP Configuration."
                    />
                  )}
                </Field>
                <div hidden={customSMTP}>
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
                  <Row>
                    <Col lg={6}>
                      <Field name="smtpData.useSSL">
                        {({ field }) => (
                          <YBToggle
                            onToggle={handleOnToggle}
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
                    </Col>
                    <Col lg={6}>
                      <Field name="smtpData.useTLS">
                        {({ field }) => (
                          <YBToggle
                            onToggle={handleOnToggle}
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
                    </Col>
                  </Row>
                </div>
              </Col>
            </Row>
          </>
        );
      default:
        return null;
    }
  };

  return (
    <YBModalForm
      initialValues={{ ALERT_TARGET_TYPE: channelType }}
      onFormSubmit={handleAddDestination}
      formName={'alertDestinationForm'}
      title="Create a new Alert Destination"
      id="alert-destination-modal"
      visible={visible}
      onHide={onModalHide}
      submitLabel={'Create'}
    >
      <Row>
        <Row>
          <Col lg={8}>
            <div className="form-item-custom-label">Target</div>
            <YBSelectWithLabel
              name={'ALERT_TARGET_TYPE'}
              options={targetTypeList}
              onInputChanged={handleChannelTypeChange}
            />
          </Col>
        </Row>
        <hr />
        {getChannelForm()}
      </Row>
    </YBModalForm>
  );
};
