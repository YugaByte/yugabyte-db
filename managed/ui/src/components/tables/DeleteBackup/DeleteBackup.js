// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { YBModal } from '../../common/forms/fields';
import PropTypes from 'prop-types';
import { isNonEmptyObject } from '../../../utils/ObjectUtils';

export default class DeleteBackup extends Component {
  static propTypes = {
    tableInfo: PropTypes.object
  };

  confirmDeleteBackup = async () => {
    const {
      tableInfo: { backupUUID },
      deleteBackup,
      onHide,
      onSubmit,
      onError
    } = this.props;
    try {
      const response = await deleteBackup(backupUUID);
      onSubmit(response.data);
    } catch (err) {
      if (onError) {
        onError();
      }
    }
    onHide();
  };

  render() {
    if (!isNonEmptyObject(this.props.tableInfo)) {
      return <span />;
    }
    const {
      visible,
      onHide,
    } = this.props;

    return (
      <div className="universe-apps-modal">
        <YBModal
          title={'Delete Backup'}
          visible={visible}
          onHide={onHide}
          showCancelButton={true}
          cancelLabel={'Cancel'}
          onFormSubmit={this.confirmDeleteBackup}
        >
          Are you sure you want to delete this backup?
        </YBModal>
      </div>
    );
  }
}
