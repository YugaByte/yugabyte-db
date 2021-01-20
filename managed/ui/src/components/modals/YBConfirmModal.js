// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { YBModal } from '../common/forms/fields';

export default class YBConfirmModal extends Component {
  static propTypes = {
    name: PropTypes.string.isRequired,
    title: PropTypes.string.isRequired,
    onConfirm: PropTypes.func.isRequired,
    confirmLabel: PropTypes.string,
    cancelLabel: PropTypes.string
  };

  static defaultProps = {
    confirmLabel: 'Confirm',
    cancelLabel: 'Cancel'
  };

  submitConfirmModal = () => {
    const { onConfirm, hideConfirmModal } = this.props;
    if (onConfirm) {
      onConfirm();
    }
    hideConfirmModal();
  };

  render() {
    const { name, title, confirmLabel, cancelLabel, hideConfirmModal } = this.props;
    return (
      <div className={name} key={name}>
        <YBModal
          title={title}
          visible={this.props.visibleModal === this.props.currentModal}
          onHide={hideConfirmModal}
          showCancelButton={true}
          cancelLabel={cancelLabel}
          submitLabel={confirmLabel}
          onFormSubmit={this.submitConfirmModal}
          submitOnCarriage
        >
          {this.props.children}
        </YBModal>
      </div>
    );
  }
}
