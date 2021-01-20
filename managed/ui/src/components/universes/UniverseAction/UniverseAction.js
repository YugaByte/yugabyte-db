// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { connect } from 'react-redux';
import moment from 'moment';

import { YBModalForm } from '../../../components/common/forms';
import { YBButton } from '../../common/forms/fields';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { AlertSnoozeModal } from '../../universes';

import {
  setAlertsConfig,
  setAlertsConfigResponse,
  updateBackupState,
  updateBackupStateResponse,
  fetchUniverseInfo,
  fetchUniverseInfoResponse
} from '../../../actions/universe';

class UniverseAction extends Component {
  constructor(props) {
    super(props);
    this.state = {
      showModal: false
    };
  }
  openModal = () => {
    this.setState((prevState, props) => {
      return {
        showModal: true
      };
    });
  };
  closeModal = () => {
    this.setState((prevState, props) => {
      return {
        showModal: false
      };
    });
  };
  UNSAFE_componentWillReceiveProps(nextProps) {
    if (
      getPromiseState(nextProps.backupState).isSuccess() ||
      getPromiseState(nextProps.alertsConfig).isSuccess()
    ) {
      this.closeModal();
      this.props.fetchCurrentUniverse(nextProps.universe.universeUUID);
    }
  }

  performAction = (values) => {
    const { universe, actionType, updateBackupState, setAlertsConfig } = this.props;
    switch (actionType) {
      case 'toggle-backup':
        const takeBackups =
          universe.universeConfig && universe.universeConfig.takeBackups === 'true';
        updateBackupState(universe.universeUUID, !takeBackups);
        break;
      case 'alert-config':
        setAlertsConfig(universe.universeUUID, values);
        break;
      default:
        break;
    }
  };

  render() {
    const {
      btnClass,
      disabled,
      actionType,
      universe: { universeConfig }
    } = this.props;
    let btnLabel = null;
    let btnIcon = null;
    let modalTitle = null;
    let modalForm = null;
    switch (actionType) {
      case 'toggle-backup':
        const takeBackups = universeConfig && universeConfig.takeBackups === 'true';
        btnLabel = takeBackups ? 'Disable Backup' : 'Enable Backup';
        btnIcon = takeBackups ? 'fa fa-pause' : 'fa fa-play';
        modalTitle = `${btnLabel} for: ${this.props.universe.name}?`;
        modalForm = (
          <YBModalForm
            title={modalTitle}
            visible={this.state.showModal}
            onHide={this.closeModal}
            showCancelButton={true}
            cancelLabel={'Cancel'}
            submitLabel={'Yes'}
            className="universe-action-modal"
            onFormSubmit={this.performAction}
          >
            <Row>
              <Col lg={12}>Are you sure you want to perform this action?</Col>
            </Row>
          </YBModalForm>
        );
        break;
      case 'alert-config':
        let disablePeriodSecs = null;
        let alertsSnoozed = false;
        if (universeConfig) {
          // If the disableAlertsUntilSecs is null or it is 0, it means the alerts is not snoozed.
          if (
            !universeConfig.disableAlertsUntilSecs ||
            universeConfig.disableAlertsUntilSecs === '0'
          ) {
            disablePeriodSecs = 0;
          } else {
            // If it has a value, we need to check if the snooze until time has elasped
            // if so then the universe alerts aren't snoozed.
            const disabledUntil = moment.unix(universeConfig.disableAlertsUntilSecs);
            if (disabledUntil.isValid()) {
              disablePeriodSecs = universeConfig.disableAlertsUntilSecs - moment().unix();
              alertsSnoozed = disablePeriodSecs > 0;
            } else {
              // If the disable alert until seconds is a invalid timestamp,
              // it means the alerts was snoozed indefinitely.
              alertsSnoozed = true;
            }
          }
        }
        btnLabel = alertsSnoozed ? 'Enable Alerts' : 'Snooze Alerts';
        btnIcon = alertsSnoozed ? 'fa fa-play' : 'fa fa-pause';
        modalForm = (
          <AlertSnoozeModal
            {...this.props}
            visible={this.state.showModal}
            onHide={this.closeModal}
            alertsSnoozed={alertsSnoozed}
            disablePeriodSecs={disablePeriodSecs}
            onFormSubmit={this.performAction}
          />
        );
        break;
      default:
        break;
    }
    return (
      <div>
        <YBButton
          btnText={btnLabel}
          btnIcon={btnIcon}
          btnClass={`btn ${btnClass}`}
          disabled={disabled}
          onClick={disabled ? null : this.openModal}
        />
        {modalForm}
      </div>
    );
  }
}

const mapDispatchToProps = (dispatch) => {
  return {
    setAlertsConfig: (uuid, payload) => {
      dispatch(setAlertsConfig(uuid, payload)).then((response) => {
        dispatch(setAlertsConfigResponse(response.payload));
      });
    },
    updateBackupState: (uuid, flag) => {
      dispatch(updateBackupState(uuid, flag)).then((response) => {
        dispatch(updateBackupStateResponse(response.payload));
      });
    },

    fetchCurrentUniverse: (universeUUID) => {
      dispatch(fetchUniverseInfo(universeUUID)).then((response) => {
        dispatch(fetchUniverseInfoResponse(response.payload));
      });
    }
  };
};

function mapStateToProps(state, ownProps) {
  return {
    alertsConfig: state.universe.alertsConfig,
    backupState: state.universe.backupState
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(UniverseAction);
