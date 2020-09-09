// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import {Field} from 'redux-form';
import {Row, Col} from 'react-bootstrap';
import {YBInputField, YBButton, YBTextArea, YBNumericInput} from '../../../common/forms/fields';
import constants from './OnPremWizardConstants.json';
import YBToggle from "../../../common/forms/fields/YBToggle";

export default class OnPremProviderAndAccessKey extends Component {
  constructor(props) {
    super(props);
    this.state  = {privateKeyFile: {}};
  }

  submitProviderKeyForm = vals => {
    this.props.setOnPremProviderAndAccessKey(vals);
  };

  privateKeyUpload = val => {
    this.setState({privateKeyFile: val[0]});
  };

  render() {
    const {handleSubmit, switchToJsonEntry, isEditProvider} = this.props;
    const {nameHelpContent, userHelpContent, pkHelpContent,
      passwordlessSudoHelp, airGapInstallHelp, homeDirHelp,
      portHelpContent, useHostnameHelp} = constants;
    const isReadOnly = this.props.isEditProvider;

    return (
      <div className="on-prem-provider-form-container">
        <form name="onPremConfigForm" onSubmit={handleSubmit(this.submitProviderKeyForm)}>
          <Row>
            <Col lg={6}>
              <div className="form-right-aligned-labels">
                <Field name="name" component={YBInputField} label="Provider Name" insetError={true}
                  isReadOnly={isReadOnly} infoContent={nameHelpContent}
                  infoTitle="Provider Name" />
                <Field name="sshUser" component={YBInputField} label="SSH User" insetError={true}
                  isReadOnly={isReadOnly} infoContent={userHelpContent}
                  infoTitle="SSH User" />
                <Field name="sshPort" component={YBNumericInput} label="SSH Port" insetError={true}
                  isReadOnly={isReadOnly} infoContent={portHelpContent}
                  infoTitle="SSH Port" />
                <Field name="useHostnames" component={YBToggle}
                  label="Use Hostnames" defaultChecked={false} isReadOnly={isReadOnly}
                  infoContent={useHostnameHelp} infoTitle="Use Hostnames"/>
                <Field name="passwordlessSudoAccess" component={YBToggle}
                  label="Passwordless Sudo" defaultChecked={true} isReadOnly={isReadOnly}
                  infoContent={passwordlessSudoHelp} infoTitle="Passwordless Sudo"/>
                <Field name="privateKeyContent" component={YBTextArea} label="SSH Key" insetError={true}
                  className="ssh-key-container" isReadOnly={isReadOnly} infoContent={pkHelpContent}
                  infoTitle="SSH Key" />
                <Field name="airGapInstall" component={YBToggle} isReadOnly={isReadOnly}
                  label="Air Gap Installation" defaultChecked={false}
                  infoContent={airGapInstallHelp} infoTitle="Air Gap Installation"/>
                <Field name="homeDir" component={YBTextArea} isReadOnly={isReadOnly}
                  label="Desired Home Directory (Optional)" insetError={true}
                  infoContent={homeDirHelp} infoTitle="Home Directory" />
              </div>
            </Col>
          </Row>
          <div className="form-action-button-container">
            {isEditProvider ? <YBButton btnText={"Cancel"} btnClass={"btn btn-default save-btn cancel-btn"} onClick={this.props.cancelEdit}/> : <span/>}
            {switchToJsonEntry}
            <YBButton btnText={"Next"} btnType={"submit"} btnClass={"btn btn-default save-btn"}/>
          </div>
        </form>
      </div>
    );
  }
}
