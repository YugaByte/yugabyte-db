// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import PropTypes from 'prop-types';
import {
  BulkImportContainer,
  CreateBackupContainer,
  RestoreBackupContainer,
  DeleteBackupContainer
} from '../../../components/tables';
import { ImportReleaseContainer, UpdateReleaseContainer } from '../../../components/releases';
import { ReleaseStateEnum } from '../../releases/UpdateRelease/UpdateRelease';
import { MenuItem } from 'react-bootstrap';
import { YBLabelWithIcon } from '../../common/descriptors';
import { YBButton } from '../../common/forms/fields';
import _ from 'lodash';

export default class TableAction extends Component {
  constructor(props) {
    super(props);
    this.state = {
      showModal: false
    };
    this.openModal = this.openModal.bind(this);
    this.closeModal = this.closeModal.bind(this);
  }

  static propTypes = {
    currentRow: PropTypes.object,
    isMenuItem: PropTypes.bool,
    btnClass: PropTypes.string,
    onModalSubmit: PropTypes.func,
    onSubmit: PropTypes.func,
    onError: PropTypes.func,
    actionType: PropTypes.oneOf([
      'drop',
      'import',
      'create-backup',
      'create-scheduled-backup',
      'restore-backup',
      'import-release',
      'active-release',
      'disable-release',
      'delete-release',
      'delete-backup'
    ])
  };

  static defaultProps = {
    isMenuItem: true,
    btnClass: 'btn-default'
  };

  openModal() {
    this.setState((prevState, props) => {
      return {
        selectedRow: props.currentRow,
        showModal: true
      };
    });
  }

  closeModal() {
    this.setState((prevState, props) => {
      return {
        showModal: false
      };
    });
  }

  render() {
    const { actionType, isMenuItem, disabled, onSubmit, onError } = this.props;
    let modalContainer = null;
    let btnLabel = null;
    let btnIcon = null;
    if (actionType === 'import') {
      btnLabel = 'Bulk Import';
      btnIcon = 'fa fa-download';
      modalContainer = (
        <BulkImportContainer
          visible={this.state.showModal}
          onHide={this.closeModal}
          tableInfo={this.state.selectedRow}
        />
      );
    } else if (actionType === 'create-scheduled-backup') {
      btnLabel = 'Create Scheduled Backup';
      btnIcon = 'fa fa-calendar-o';
      modalContainer = (
        <CreateBackupContainer
          visible={this.state.showModal}
          onHide={this.closeModal}
          tableInfo={this.state.selectedRow}
          onSubmit={onSubmit}
          onError={onError}
          isScheduled
        />
      );
    } else if (actionType === 'create-backup') {
      btnLabel = 'Create Backup';
      btnIcon = 'fa fa-upload';
      modalContainer = (
        <CreateBackupContainer
          visible={this.state.showModal}
          onHide={this.closeModal}
          tableInfo={this.state.selectedRow}
          onSubmit={onSubmit}
          onError={onError}
        />
      );
    } else if (actionType === 'restore-backup') {
      btnLabel = 'Restore Backup';
      btnIcon = 'fa fa-download';
      modalContainer = (
        <RestoreBackupContainer
          visible={this.state.showModal}
          onHide={this.closeModal}
          backupInfo={this.state.selectedRow}
          onSubmit={onSubmit}
          onError={onError}
        />
      );
    } else if (actionType === 'import-release') {
      btnLabel = 'Import';
      btnIcon = 'fa fa-upload';
      modalContainer = (
        <ImportReleaseContainer
          visible={this.state.showModal}
          onHide={this.closeModal}
          onModalSubmit={onSubmit}
        />
      );
    } else if (actionType === 'delete-backup') {
      btnLabel = 'Delete Backup';
      btnIcon = 'fa fa-trash';
      modalContainer = (
        <DeleteBackupContainer
          visible={this.state.showModal}
          onHide={this.closeModal}
          tableInfo={this.state.selectedRow}
          onSubmit={onSubmit}
          onError={onError}
        />
      );
    } else if (['disable-release', 'delete-release', 'active-release'].includes(actionType)) {
      let action;
      switch (actionType) {
        case 'disable-release':
          btnLabel = 'Disable';
          btnIcon = 'fa fa-ban';
          action = ReleaseStateEnum.DISABLED;
          break;
        case 'delete-release':
          btnLabel = 'Delete';
          btnIcon = 'fa fa-trash';
          action = ReleaseStateEnum.DELETED;
          break;
        case 'active-release':
          btnLabel = 'Active';
          btnIcon = 'fa fa-check';
          action = ReleaseStateEnum.ACTIVE;
          break;
        default:
          break;
      }
      modalContainer = (
        <UpdateReleaseContainer
          visible={this.state.showModal}
          onHide={this.closeModal}
          releaseInfo={this.state.selectedRow}
          actionType={action}
          onModalSubmit={this.props.onModalSubmit}
        />
      );
    }

    const btnId = _.uniqueId('table_action_btn_');
    if (isMenuItem) {
      return (
        <Fragment>
          <MenuItem eventKey={btnId} onClick={disabled ? null : this.openModal} disabled={disabled}>
            <YBLabelWithIcon icon={btnIcon}>{btnLabel}</YBLabelWithIcon>
          </MenuItem>
          {modalContainer}
        </Fragment>
      );
    }
    return (
      <div className={this.props.className}>
        <YBButton
          btnText={btnLabel}
          btnIcon={btnIcon}
          disabled={disabled}
          btnClass={'btn ' + this.props.btnClass}
          onClick={disabled ? null : this.openModal}
        />
        {modalContainer}
      </div>
    );
  }
}
