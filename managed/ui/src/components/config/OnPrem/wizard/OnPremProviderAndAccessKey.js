// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Field } from 'redux-form';
import { Row, Col } from 'react-bootstrap';
import { YBInputField, YBButton, YBTextArea, YBNumericInput } from '../../../common/forms/fields';
import constants from './OnPremWizardConstants.json';
import YBToggle from '../../../common/forms/fields/YBToggle';

export default class OnPremProviderAndAccessKey extends Component {
  constructor(props) {
    super(props);
    this.state = {
      privateKeyFile: {},
      installNodeExporter: props.initialValues.installNodeExporter,
      showAdvanced: false
    };
    this.toggleInstallNodeExporter = this.toggleInstallNodeExporter.bind(this);
    this.toggleShowAdvanced = this.toggleShowAdvanced.bind(this);
  }

  submitProviderKeyForm = (vals) => {
    this.props.setOnPremProviderAndAccessKey(vals);
  };

  privateKeyUpload = (val) => {
    this.setState({ privateKeyFile: val[0] });
  };

  toggleInstallNodeExporter() {
    this.setState({ installNodeExporter: !this.state.installNodeExporter });
  }

  toggleShowAdvanced() {
    this.setState({ showAdvanced: !this.state.showAdvanced });
  }

  render() {
    const { handleSubmit, switchToJsonEntry, isEditProvider } = this.props;
    const {
      nameHelpContent,
      userHelpContent,
      pkHelpContent,
      skipProvisioningHelp,
      airGapInstallHelp,
      portHelpContent,
      useHostnameHelp
    } = constants;
    const isReadOnly = this.props.isEditProvider;

    return (
      <div className="on-prem-provider-form-container">
        <form name="onPremConfigForm" onSubmit={handleSubmit(this.submitProviderKeyForm)}>
          <Row>
            <Col lg={6}>
              <div className="form-right-aligned-labels">
                <Field
                  name="name"
                  component={YBInputField}
                  label="Provider Name"
                  insetError={true}
                  isReadOnly={isReadOnly}
                  infoContent={nameHelpContent}
                  infoTitle="Provider Name"
                />
                <Field
                  name="sshUser"
                  component={YBInputField}
                  label="SSH User"
                  insetError={true}
                  isReadOnly={isReadOnly}
                  infoContent={userHelpContent}
                  infoTitle="SSH User"
                />
                <Field
                  name="sshPort"
                  component={YBNumericInput}
                  label="SSH Port"
                  insetError={true}
                  readOnly={isReadOnly}
                  infoContent={portHelpContent}
                  infoTitle="SSH Port"
                />
                <Field
                  name="skipProvisioning"
                  component={YBToggle}
                  label="Manually Provision Nodes"
                  defaultChecked={false}
                  isReadOnly={isReadOnly}
                  infoContent={skipProvisioningHelp}
                  infoTitle="Manually Provision Nodes"
                />
                <Field
                  name="privateKeyContent"
                  component={YBTextArea}
                  label="SSH Key"
                  insetError={true}
                  className="ssh-key-container"
                  isReadOnly={isReadOnly}
                  infoContent={pkHelpContent}
                  infoTitle="SSH Key"
                />
                <Field
                  name="airGapInstall"
                  component={YBToggle}
                  isReadOnly={isReadOnly}
                  label="Air Gap Install"
                  defaultChecked={false}
                  infoContent={airGapInstallHelp}
                  infoTitle="Air Gap Installation"
                />
                <Field
                  name="advanced"
                  component={YBToggle}
                  label="Advanced"
                  defaultChecked={false}
                  isReadOnly={false}
                  onToggle={this.toggleShowAdvanced}
                  checkedVal={this.state.showAdvanced}
                />
                {this.state.showAdvanced && (
                  <Field
                    name="useHostnames"
                    component={YBToggle}
                    label="Use Hostnames"
                    defaultChecked={false}
                    isReadOnly={isReadOnly}
                    infoContent={useHostnameHelp}
                    infoTitle="Use Hostnames"
                  />
                )}
                {this.state.showAdvanced && (
                  <Field
                    name="homeDir"
                    component={YBTextArea}
                    isReadOnly={isReadOnly}
                    label="Desired Home Directory"
                    insetError={true}
                    subLabel="Enter the desired home directory for YB nodes (optional)."
                  />
                )}
                {this.state.showAdvanced && (
                  <Field
                    name="nodeExporterPort"
                    component={YBNumericInput}
                    label="Node Exporter Port"
                    readOnly={isReadOnly}
                    insetError={true}
                  />
                )}
                {this.state.showAdvanced && (
                  <Field
                    name="installNodeExporter"
                    component={YBToggle}
                    label="Install Node Exporter"
                    defaultChecked={true}
                    isReadOnly={isReadOnly}
                    onToggle={this.toggleInstallNodeExporter}
                    checkedVal={this.state.installNodeExporter}
                    subLabel="Whether to install or skip installing Node Exporter."
                  />
                )}
                {this.state.showAdvanced && this.state.installNodeExporter && (
                  <Field
                    name="nodeExporterUser"
                    component={YBTextArea}
                    label="Node Exporter User"
                    isReadOnly={isReadOnly}
                    insetError={true}
                  />
                )}
              </div>
            </Col>
          </Row>
          <div className="form-action-button-container">
            {isEditProvider ? (
              <YBButton
                btnText={'Cancel'}
                btnClass={'btn btn-default save-btn cancel-btn'}
                onClick={this.props.cancelEdit}
              />
            ) : (
              <span />
            )}
            {switchToJsonEntry}
            <YBButton btnText={'Next'} btnType={'submit'} btnClass={'btn btn-default save-btn'} />
          </div>
        </form>
      </div>
    );
  }
}
