// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import { connect } from 'react-redux';
import { Row, Col } from 'react-bootstrap';
import { YBButton, YBAddRowButton, YBToggle, YBNumericInput } from '../../../common/forms/fields';
import { YBTextInputWithLabel, YBSelectWithLabel, YBDropZone, YBInputField } from '../../../common/forms/fields';
import { change, Field } from 'redux-form';
import { getPromiseState } from '../../../../utils/PromiseUtils';
import { YBLoading } from '../../../common/indicators';
import { isNonEmptyObject, isNonEmptyString } from '../../../../utils/ObjectUtils';
import { reduxForm, FieldArray } from 'redux-form';
import { FlexContainer, FlexGrow, FlexShrink } from '../../../common/flexbox/YBFlexBox';

const validationIsRequired = value => value && value.trim() !== '' ? undefined : 'Required';

class renderRegionInput extends Component {
  componentDidMount() {
    if (this.props.fields.length === 0) {
      this.props.fields.push({});
    }
  }
  render() {
    const { fields } = this.props;
    const regionMappingList = fields.map((item, idx) => (
      <FlexContainer key={idx}>
        <FlexGrow>
          <Row>
            <Col lg={6}>
              <Field name={`${item}.region`} validate={validationIsRequired} component={YBInputField} placeHolder="Region Name"/>
            </Col>
            <Col lg={6}>
              <Field name={`${item}.subnet`} validate={validationIsRequired} component={YBInputField} placeHolder="Subnet ID"/>
            </Col>
          </Row>
        </FlexGrow>
        <FlexShrink>
          <i className="fa fa-times fa-fw delete-row-btn" onClick={() => fields.getAll().length > 1 ? fields.remove(idx) :  null}/>
        </FlexShrink>
      </FlexContainer>
    ));
    return (
      <Fragment>
        <div className="divider"></div>
        <h5>Region mapping</h5>
        <div className="form-field-grid">
          {regionMappingList}
          <YBAddRowButton btnText="Add region" onClick={() => fields.push({})} />
        </div>
      </Fragment>
    );
  }
}

class GCPProviderInitView extends Component {
  constructor(props) {
    super(props);
    this.state = {
      gcpConfig: {},
      accountName: "Google Cloud Provider",
      providerUUID: "",
      currentProvider: {},
      hostVpcVisible: true,
      networkSetupType: "new_vpc",
      credentialInputType: "upload_service_account_json"
    };
    this.hostVpcToggled = this.hostVpcToggled.bind(this);
  }

  createProviderConfig = vals => {
    const self = this;
    const gcpCreateConfig = {};
    const perRegionMetadata = {};
    if (isNonEmptyString(vals.destVpcId)) {
      gcpCreateConfig["network"] = vals.destVpcId;
      gcpCreateConfig["use_host_vpc"] = true;
    } else {
      gcpCreateConfig["use_host_vpc"] = false;
    }
    if (isNonEmptyString(vals.gcpProjectName)) {
      gcpCreateConfig["host_project_id"] = vals.gcpProjectName;
    }
    if (vals.network_setup !== "new_vpc") {
      vals.regionMapping.forEach((item) =>
        perRegionMetadata[item.region] = { "subnetId": item.subnet}
      );
    }
    if (isNonEmptyString(vals.firewall_tags)) {
      gcpCreateConfig["YB_FIREWALL_TAGS"] = vals.firewall_tags;
    }
    gcpCreateConfig["airGapInstall"] = vals.airGapInstall;
    gcpCreateConfig["sshPort"] = vals.sshPort;
    const providerName = vals.accountName;
    const configText = vals.gcpConfig;
    if (vals.credential_input === "local_service_account") {
      gcpCreateConfig["use_host_credentials"] = true;
      return self.props.createGCPProvider(providerName, gcpCreateConfig, perRegionMetadata);
    } else if (vals.credential_input === "upload_service_account_json" && isNonEmptyObject(configText)) {
      gcpCreateConfig["use_host_credentials"] = false;
      const reader = new FileReader();
      reader.readAsText(configText);
      // Parse the file back to JSON, since the API controller endpoint doesn't support file upload
      reader.onloadend = function () {
        try {
          gcpCreateConfig["config_file_contents"] = JSON.parse(reader.result);
        } catch (e) {
          self.setState({"error": "Invalid GCP config JSON file"});
        }
        return self.props.createGCPProvider(providerName, gcpCreateConfig, perRegionMetadata);
      };
    } else {
      this.setState({"error": "GCP Config JSON is required"});
    }
  };

  isHostInGCP() {
    const { hostInfo } = this.props;
    return isNonEmptyObject(hostInfo) && isNonEmptyObject(hostInfo["gcp"]) &&
      hostInfo["gcp"]["error"] === undefined;
  }

  uploadGCPConfig(uploadFile) {
    this.setState({gcpConfig: uploadFile[0]});
  }

  hostVpcToggled(event) {
    this.setState({hostVpcVisible: !event.target.checked});
  }

  networkSetupChanged = (value) => {
    const { hostInfo } = this.props;
    if (value === "host_vpc") {
      this.updateFormField("destVpcId", hostInfo["gcp"]["network"]);
      this.updateFormField("gcpProjectName", hostInfo["gcp"]["host_project"]);
    } else {
      this.updateFormField("destVpcId", null);
      this.updateFormField("gcpProjectName", null);
    }
    this.setState({networkSetupType: value});
  }
  credentialInputChanged = (value) => {
    this.setState({credentialInputType: value});
  }

  updateFormField = (field, value) => {
    this.props.dispatch(change("gcpProviderConfigForm", field, value));
  };

  render() {
    const { handleSubmit, configuredProviders, submitting } = this.props;
    if (getPromiseState(configuredProviders).isLoading()) {
      return <YBLoading />;
    }
    const network_setup_options = [
      <option key={1} value={"new_vpc"}>{"Create a new VPC"}</option>,
      <option key={2} value={"existing_vpc"}>{"Specify an existing VPC"}</option>
    ];
    if (this.isHostInGCP()) {
      network_setup_options.push(
        <option key={3} value={"host_vpc"}>{"Use VPC of the Admin Console instance"}</option>
      );
    }
    const credential_input_options = [
      <option key={1} value={"upload_service_account_json"}>{"Upload Service Account config"}</option>,
      <option key={2} value={"local_service_account"}>{"Use Service Account on instance"}</option>
    ];
    let uploadConfigField = <span />;
    if (this.state.credentialInputType === "upload_service_account_json") {
      let gcpConfigFileName = "";
      if (isNonEmptyObject(this.state.gcpConfig)) {
        gcpConfigFileName = this.state.gcpConfig.name;
      }
      uploadConfigField = (
        <Row className="config-provider-row">
          <Col lg={3}>
            <div className="form-item-custom-label">Provider Config</div>
          </Col>
          <Col lg={7}>
            <Field name="gcpConfig" component={YBDropZone} className="upload-file-button" title={"Upload GCP Config json file"}/>
          </Col>
          <Col lg={4}>
            <div className="file-label">{gcpConfigFileName}</div>
          </Col>
        </Row>
      );
    }

    let destVpcField = <span />, gcpProjectField = <span />, regionInput = <span />;
    if (this.state.networkSetupType !== "new_vpc") {
      destVpcField = (
        <Row className="config-provider-row">
          <Col lg={3}>
            <div className="form-item-custom-label">
              VPC Network Name
            </div>
          </Col>
          <Col lg={7}>
            <Field name="destVpcId" component={YBTextInputWithLabel}
                placeHolder="my-vpc-network-name"
                className={"gcp-provider-input-field"}
                isReadOnly={this.state.networkSetupType === "host_vpc"}/>
          </Col>
        </Row>
      );
      gcpProjectField = (
        <Row className="config-provider-row">
          <Col lg={3}>
            <div className="form-item-custom-label">
              Host Project Name
            </div>
          </Col>
          <Col lg={7}>
            <Field name="gcpProjectName" component={YBTextInputWithLabel}
                placeHolder="my-gcp-project-name"
                className={"gcp-provider-input-field"}
                isReadOnly={this.state.networkSetupType === "host_vpc"}/>
          </Col>
        </Row>
      );

      regionInput = (<FieldArray name={'regionMapping'} component={renderRegionInput} />);
    }

    return (
      <div className="provider-config-container">
        <form name="gcpProviderConfigForm" onSubmit={handleSubmit(this.createProviderConfig)}>
          <div className="editor-container">
            <Row className="config-section-header">
              <Col lg={8}>
                <Row className="config-provider-row">
                  <Col lg={3}>
                    <div className="form-item-custom-label">Name</div>
                  </Col>
                  <Col lg={7}>
                    <Field name="accountName" placeHolder="Google Cloud Platform"
                           component={YBTextInputWithLabel} className={"gcp-provider-input-field"}/>
                  </Col>
                </Row>
                <Row>
                  <Col lg={3}>
                    <div className="form-item-custom-label">Credential Type</div>
                  </Col>
                  <Col lg={7}>
                    <Field name="credential_input" component={YBSelectWithLabel}
                      options={credential_input_options}
                      onInputChanged={this.credentialInputChanged} />
                  </Col>
                </Row>
                {uploadConfigField}
                <Row>
                  <Col lg={3}>
                    <div className="form-item-custom-label">Air Gap Installation</div>
                  </Col>
                  <Col lg={7}>
                    <Field name="airGapInstall" component={YBToggle}/>
                  </Col>
                </Row>
                <Row>
                  <Col lg={3}>
                    <div className="form-item-custom-label">SSH Port</div>
                  </Col>
                  <Col lg={7}>
                    <Field name="sshPort" component={YBNumericInput}/>
                  </Col>
                </Row>
                <Row>
                  <Col lg={3}>
                    <div className="form-item-custom-label">VPC Setup</div>
                  </Col>
                  <Col lg={7}>
                    <Field name="network_setup" component={YBSelectWithLabel}
                      options={network_setup_options}
                      onInputChanged={this.networkSetupChanged} />
                  </Col>
                </Row>
                <Row>
                  <Col lg={3}>
                    <div className="form-item-custom-label">Firewall Tags</div>
                  </Col>
                  <Col lg={7}>
                    <Field name="firewall_tags" placeHolder="my-firewall-tag-1,my-firewall-tag-2"
                           component={YBTextInputWithLabel} className={"gcp-provider-input-field"}/>
                  </Col>
                </Row>
                {gcpProjectField}
                {destVpcField}
                {regionInput}
              </Col>
            </Row>
          </div>
          <div className="form-action-button-container">
            <YBButton btnText={"Save"} disabled={submitting} btnClass={"btn btn-default save-btn"} btnType="submit"/>
          </div>
        </form>
      </div>
    );
  }
}

const validate = (values) => {
  const errors = {};
  if (!isNonEmptyString(values.accountName)) {
    errors.accountName = 'Account Name is Required';
  }
  if (!isNonEmptyObject(values.gcpConfig)) {
    errors.gcpConfig = 'Provider Config is Required';
  }
  if (values.network_setup === "existing_vpc") {
    if (!isNonEmptyString(values.gcpProjectName)) {
      errors.gcpProjectName = 'Project Name is Required';
    }
    if (!isNonEmptyString(values.destVpcId)) {
      errors.destVpcId = 'VPC Network Name is Required';
    }
  }
  return errors;
};

function mapStateToProps(state) {
  return {
    initialValues: {
      accountName: '',
      credential_input: 'upload_service_account_json',
      airGapInstall: false,
      network_setup: 'new_vpc'
    }
  };
}

export default connect(mapStateToProps, null)(reduxForm({
  form: 'gcpProviderConfigForm',
  validate
})(GCPProviderInitView));
