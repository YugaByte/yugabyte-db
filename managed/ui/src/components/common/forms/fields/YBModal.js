// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { Modal } from 'react-bootstrap';
import YBButton from './YBButton';
import './stylesheets/YBModal.scss';

const ENTER_KEY_CODE = 13;
const ESC_KEY_CODE = 27;

export default class YBModal extends Component {
  handleKeyPressFunction = (event) => {
    const { onHide, submitOnCarriage, onFormSubmit } = this.props;
    if (event.keyCode === ESC_KEY_CODE) {
      onHide(event);
    } else if (event.keyCode === ENTER_KEY_CODE && submitOnCarriage) {
      onFormSubmit();
    }
  };

  componentDidMount() {
    document.addEventListener('keydown', this.handleKeyPressFunction, false);
  }
  componentWillUnmount() {
    document.removeEventListener('keydown', this.handleKeyPressFunction, false);
  }

  render() {
    const {
      visible,
      onHide,
      size,
      formName,
      onFormSubmit,
      title,
      submitLabel,
      cancelLabel,
      error,
      submitting,
      asyncValidating,
      footerAccessory,
      showCancelButton,
      className,
      normalizeFooter,
      disableSubmit
    } = this.props;
    let btnDisabled = false;
    if (submitting || asyncValidating || disableSubmit) {
      btnDisabled = true;
    }
    let footerButtonClass = '';
    if (normalizeFooter) {
      footerButtonClass = 'modal-action-buttons';
    }
    return (
      <Modal show={visible} onHide={onHide} bsSize={size} className={className}>
        <form name={formName} onSubmit={onFormSubmit}>
          <Modal.Header closeButton>
            <Modal.Title>{title}</Modal.Title>
            <div
              className={`yb-alert-item
                ${error ? '' : 'hide'}`}
            >
              {error}
            </div>
          </Modal.Header>
          <Modal.Body>{this.props.children}</Modal.Body>
          {(footerAccessory || showCancelButton || onFormSubmit) && (
            <Modal.Footer>
              <div className={footerButtonClass}>
                {onFormSubmit && (
                  <YBButton
                    btnClass="btn btn-orange pull-right"
                    disabled={btnDisabled}
                    btnText={submitLabel}
                    onClick={onFormSubmit}
                  />
                )}
                {showCancelButton && (
                  <YBButton btnClass="btn" btnText={cancelLabel} onClick={onHide} />
                )}
                {footerAccessory && (
                  <div className="pull-left modal-accessory">{footerAccessory}</div>
                )}
              </div>
            </Modal.Footer>
          )}
        </form>
      </Modal>
    );
  }
}

YBModal.propTypes = {
  title: PropTypes.oneOfType([
    PropTypes.string,
    PropTypes.object
  ]).isRequired,
  visible: PropTypes.bool,
  size: PropTypes.oneOf(['large', 'small', 'xsmall']),
  formName: PropTypes.string,
  onFormSubmit: PropTypes.func,
  onHide: PropTypes.func,
  submitLabel: PropTypes.string,
  cancelLabel: PropTypes.string,
  footerAccessory: PropTypes.object,
  showCancelButton: PropTypes.bool
};

YBModal.defaultProps = {
  visible: false,
  submitLabel: 'OK',
  cancelLabel: 'Cancel',
  showCancelButton: false
};
