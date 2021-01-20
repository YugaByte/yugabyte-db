// Copyright (c) YugaByte, Inc.

import _ from 'lodash';
import React, { Component } from 'react';
import { Field, FieldArray } from 'redux-form';
import { Row, Col, Tabs, Tab, Alert } from 'react-bootstrap';
import {
  YBModal,
  YBInputField,
  YBAddRowButton,
  YBSelectWithLabel,
  YBToggle,
  YBCheckBox,
  YBRadioButtonBarWithLabel
} from '../fields';
import { isNonEmptyArray } from '../../../../utils/ObjectUtils';
import { getPromiseState } from '../../../../utils/PromiseUtils';
import { getPrimaryCluster } from '../../../../utils/UniverseUtils';
import { isDefinedNotNull, isNonEmptyObject } from '../../../../utils/ObjectUtils';
import './RollingUpgradeForm.scss';

class FlagInput extends Component {
  render() {
    const { deleteRow, item } = this.props;
    return (
      <Row>
        <Col lg={5}>
          <Field
            name={`${item}.name`}
            component={YBInputField}
            className="input-sm"
            placeHolder="Flag Name"
          />
        </Col>
        <Col lg={5}>
          <Field
            name={`${item}.value`}
            component={YBInputField}
            className="input-sm"
            placeHolder="Value"
          />
        </Col>
        <Col lg={1}>
          <i className="fa fa-times fa-fw delete-row-btn" onClick={deleteRow} />
        </Col>
      </Row>
    );
  }
}

class FlagItems extends Component {
  componentDidMount() {
    if (this.props.fields.length === 0) {
      this.props.fields.push({});
    }
  }
  render() {
    const { fields } = this.props;
    const addFlagItem = () => fields.push({});
    const gFlagsFieldList = fields.map((item, idx) => (
      <FlagInput item={item} key={idx} deleteRow={() => fields.remove(idx)} />
    ));

    return (
      <div className="form-field-grid">
        {gFlagsFieldList}
        <YBAddRowButton btnText="Add" onClick={addFlagItem} />
      </div>
    );
  }
}

export default class RollingUpgradeForm extends Component {
  constructor(props) {
    super(props);
    this.state = { formConfirmed: false };
  }

  toggleConfirmValidation = () => {
    this.setState({ formConfirmed: !this.state.formConfirmed });
  };

  componentWillUnmount() {
    this.resetAndClose();
  }

  getCurrentVersion = () => {
    const { universe } = this.props;
    let currentVersion = null;
    if (
      isDefinedNotNull(universe.currentUniverse.data) &&
      isNonEmptyObject(universe.currentUniverse.data)
    ) {
      const primaryCluster = getPrimaryCluster(
        universe.currentUniverse.data.universeDetails.clusters
      );
      currentVersion = primaryCluster?.userIntent?.ybSoftwareVersion;
    }
    return currentVersion;
  }

  setRollingUpgradeProperties = (values) => {
    const {
      modal: { visibleModal },
      universe: {
        currentUniverse: {
          data: {
            universeDetails: { clusters, nodePrefix },
            universeUUID
          }
        }
      }
    } = this.props;

    const payload = {};
    switch (visibleModal) {
      case 'softwareUpgradesModal': {
        payload.taskType = 'Software';
        payload.upgradeOption = values.rollingUpgrade ? 'Rolling' : 'Non-Rolling';
        break;
      }
      case 'gFlagsModal': {
        payload.taskType = 'GFlags';
        payload.upgradeOption = values.upgradeOption;
        break;
      }
      case 'tlsConfigurationModal': {
        payload.taskType = 'Certs';
        payload.upgradeOption = values.rollingUpgrade ? 'Rolling' : 'Non-Rolling';
        payload.certUUID = values.tlsCertificate;
        break;
      }
      case 'rollingRestart': {
        payload.taskType = 'Restart';
        payload.upgradeOption = 'Rolling';
        break;
      }
      default: return;
    }

    const primaryCluster = _.cloneDeep(getPrimaryCluster(clusters));
    if (!isDefinedNotNull(primaryCluster)) {
      return;
    }
    payload.ybSoftwareVersion = values.ybSoftwareVersion;
    payload.universeUUID = universeUUID;
    payload.nodePrefix = nodePrefix;
    let masterGFlagList = [];
    let tserverGFlagList = [];
    if (isNonEmptyArray(values.masterGFlags)) {
      masterGFlagList = values.masterGFlags
        .map((masterFlag) => {
          return (masterFlag.name && masterFlag.value)
            ? { name: masterFlag.name, value: masterFlag.value }
            : null;
        })
        .filter(Boolean);
    }
    if (isNonEmptyArray(values.tserverGFlags)) {
      tserverGFlagList = values.tserverGFlags
        .map((tserverFlag) => {
          return (tserverFlag.name && tserverFlag.value)
            ? { name: tserverFlag.name, value: tserverFlag.value }
            : null;
        })
        .filter(Boolean);
    }
    primaryCluster.userIntent.ybSoftwareVersion = values.ybSoftwareVersion;
    primaryCluster.userIntent.masterGFlags = masterGFlagList;
    primaryCluster.userIntent.tserverGFlags = tserverGFlagList;
    payload.clusters = [primaryCluster];
    payload.sleepAfterMasterRestartMillis = values.timeDelay * 1000;
    payload.sleepAfterTServerRestartMillis = values.timeDelay * 1000;

    this.props.submitRollingUpgradeForm(payload, universeUUID).then(response => {
      if (response.payload.status === 200) {
        this.props.fetchCurrentUniverse(universeUUID);
        this.props.fetchUniverseMetadata();
        this.props.fetchCustomerTasks();
        this.props.fetchUniverseTasks(universeUUID);
        this.resetAndClose();
      }
    });
  };

  resetAndClose = () => {
    this.props.onHide();
    this.props.reset();
    this.props.resetRollingUpgrade();
    this.setState({ formConfirmed: false });
  };

  render() {
    const {
      modalVisible,
      handleSubmit,
      universe,
      certificates,
      modal: { visibleModal },
      universe: { error },
      softwareVersions,
      formValues
    } = this.props;

    const currentVersion = this.getCurrentVersion();
    const submitAction = handleSubmit(this.setRollingUpgradeProperties);

    const softwareVersionOptions = softwareVersions.map((item, idx) => (
      <option key={idx} disabled={item === currentVersion} value={item}>
        {item}
      </option>
    ));
    const tlsCertificateOptions = certificates.map(item => (
      <option
        key={item.uuid}
        disabled={item.uuid === universe.currentUniverse?.data?.universeDetails?.rootCA}
        value={item.uuid}
      >
        {item.label}
      </option>
    ));

    const errorAlert = getPromiseState(universe.rollingUpgrade).isError() && (
      <Alert bsStyle="danger" variant="danger">
        {universe.rollingUpgrade.error
          ? JSON.stringify(universe.rollingUpgrade.error)
          : 'Something went wrong'}
      </Alert>
    );

    switch (visibleModal) {
      case 'softwareUpgradesModal': {
        return (
          <YBModal
            className={getPromiseState(universe.rollingUpgrade).isError() ? 'modal-shake' : ''}
            visible={modalVisible}
            formName="RollingUpgradeForm"
            onHide={this.resetAndClose}
            submitLabel="Upgrade"
            showCancelButton
            title="Upgrade Software"
            onFormSubmit={submitAction}
            error={error}
            footerAccessory={
              formValues.ybSoftwareVersion !== currentVersion
                ? (
                  <YBCheckBox
                    label="Confirm software upgrade"
                    input={{ checked: this.state.formConfirmed, onChange: this.toggleConfirmValidation }}
                  />
                )
                : <span>Latest software is installed</span>
            }
            asyncValidating={!this.state.formConfirmed}
          >
            <Col lg={12} className="form-section-title">
              Software Package Version
            </Col>
            <div className="form-right-aligned-labels rolling-upgrade-form">
              <Field name="rollingUpgrade" component={YBToggle} label="Rolling Upgrade" />
              <Field
                name="timeDelay"
                type="number"
                component={YBInputField}
                label="Upgrade Delay Between Servers (secs)"
              />
              <Field
                name="ybSoftwareVersion"
                type="select"
                component={YBSelectWithLabel}
                options={softwareVersionOptions}
                label="Server Version"
              />
            </div>
            {errorAlert}
          </YBModal>
        );
      }
      case 'gFlagsModal': {
        return (
          <YBModal
            className={getPromiseState(universe.rollingUpgrade).isError() ? 'modal-shake' : ''}
            visible={modalVisible}
            formName="RollingUpgradeForm"
            onHide={this.resetAndClose}
            title="Flags"
            onFormSubmit={submitAction}
            error={error}
          >
            <Tabs defaultActiveKey={1} className="gflag-display-container" id="gflag-container">
              <Tab eventKey={1} title="Master" className="gflag-class-1" bsClass="gflag-class-2">
                <FieldArray
                  name="masterGFlags"
                  component={FlagItems}
                />
              </Tab>
              <Tab eventKey={2} title="T-Server">
                <FieldArray
                  name="tserverGFlags"
                  component={FlagItems}
                />
              </Tab>
            </Tabs>
            <div className="form-right-aligned-labels rolling-upgrade-form top-10 time-delay-container">
              <Field
                name="upgradeOption"
                component={YBRadioButtonBarWithLabel}
                options={['Rolling', 'Non-Rolling', 'Non-Restart']}
                label="Upgrade Option"
                initialValue="Rolling"
              />
              <Field
                name="timeDelay"
                type="number"
                component={YBInputField}
                label="Rolling Upgrade Delay Between Servers (secs)"
                isReadOnly={formValues.upgradeOption !== 'Rolling'}
              />
            </div>
            {errorAlert}
          </YBModal>
        );
      }
      case 'tlsConfigurationModal': {
        return (
          <YBModal
            className={getPromiseState(universe.rollingUpgrade).isError() ? 'modal-shake' : ''}
            visible={modalVisible}
            formName="RollingUpgradeForm"
            onHide={this.resetAndClose}
            submitLabel="Upgrade"
            showCancelButton
            title="Edit TLS Configuration"
            onFormSubmit={submitAction}
            error={error}
            footerAccessory={
              formValues.tlsCertificate !== universe.currentUniverse?.data?.universeDetails?.rootCA
                ? (
                  <YBCheckBox
                    label="Confirm TLS Changes"
                    input={{ checked: this.state.formConfirmed, onChange: this.toggleConfirmValidation }}
                  />
                )
                : <span>Select new CA signed cert from the list</span>
            }
            asyncValidating={
              !this.state.formConfirmed ||
              formValues.tlsCertificate === universe.currentUniverse?.data?.universeDetails?.rootCA
            }
          >
            <div className="form-right-aligned-labels rolling-upgrade-form">
              <Field
                name="tlsCertificate"
                type="select"
                component={YBSelectWithLabel}
                options={tlsCertificateOptions}
                label="New TLS Certificate"
              />
              <Field
                name="timeDelay"
                type="number"
                required
                component={YBInputField}
                label="Upgrade Delay Between Servers (secs)"
              />
              <Field name="rollingUpgrade" component={YBToggle} label="Rolling Upgrade" />
            </div>
            {errorAlert}
          </YBModal>
        );
      }
      case 'rollingRestart': {
        return (
          <YBModal
            className={getPromiseState(universe.rollingUpgrade).isError() ? 'modal-shake' : ''}
            visible={modalVisible}
            formName="RollingUpgradeForm"
            onHide={this.resetAndClose}
            submitLabel="Restart"
            showCancelButton
            title="Initiate Rolling Restart"
            onFormSubmit={submitAction}
            error={error}
            footerAccessory={
              <YBCheckBox
                label="Confirm rolling restart"
                input={{ checked: this.state.formConfirmed, onChange: this.toggleConfirmValidation }}
              />
            }
            asyncValidating={!this.state.formConfirmed}
          >
            <div className="form-right-aligned-labels rolling-upgrade-form">
              <Field
                name="timeDelay"
                type="number"
                component={YBInputField}
                label="Rolling Restart Delay Between Servers (secs)"
              />
            </div>
            {errorAlert}
          </YBModal>
        );
      }
      default: {
        return null;
      }
    }
  }
}
