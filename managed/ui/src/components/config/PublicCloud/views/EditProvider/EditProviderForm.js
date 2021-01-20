// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Alert } from 'react-bootstrap';
import { YBButton, YBTextInputWithLabel } from '../../../../common/forms/fields';
import { Field } from 'redux-form';
import { isNonEmptyObject, isNonEmptyString } from '../../../../../utils/ObjectUtils';
import { getPromiseState } from '../../../../../utils/PromiseUtils';

export default class EditProviderForm extends Component {
  submitEditProvider = (payload) => {
    this.props.submitEditProvider(payload);
  };

  switchToResultView = () => {
    this.props.switchToResultView();
  };

  componentDidUpdate(prevProps) {
    const { editProvider } = this.props;
    if (
      isNonEmptyObject(editProvider) &&
      getPromiseState(editProvider).isSuccess() &&
      getPromiseState(prevProps.editProvider).isLoading()
    ) {
      this.props.reloadCloudMetadata();
      this.props.switchToResultView();
    }
  }

  render() {
    const { error, handleSubmit, hostedZoneId } = this.props;
    let isHostedZoneIdValid = true;
    let verifyEditConditions = true;
    if (!isNonEmptyString(hostedZoneId)) {
      isHostedZoneIdValid = false;
      verifyEditConditions = false;
    }
    return (
      <div>
        <h4>Edit Provider</h4>
        <form name="EditProviderForm" onSubmit={handleSubmit(this.submitEditProvider)}>
          {error && <Alert bsStyle="danger">{error}</Alert>}
          <div className="aws-config-form form-right-aligned-labels">
            <Field
              name="accountName"
              type="text"
              label="Name"
              component={YBTextInputWithLabel}
              isReadOnly={true}
            />
            <Field
              name="accountUUID"
              type="text"
              label="Provider UUID"
              component={YBTextInputWithLabel}
              isReadOnly={true}
            />
            <Field
              name="secretKey"
              type="text"
              label="SSH Key"
              component={YBTextInputWithLabel}
              isReadOnly={true}
            />
            <Field
              name="hostedZoneId"
              type="text"
              label="Hosted Zone ID"
              component={YBTextInputWithLabel}
              isReadOnly={isHostedZoneIdValid}
            />
            <div className="form-action-button-container">
              <YBButton
                btnText={'Submit'}
                btnClass={'btn btn-default save-btn'}
                btnType="submit"
                disabled={verifyEditConditions}
              />
              <YBButton
                btnText={'Cancel'}
                btnClass={'btn btn-default save-btn'}
                onClick={this.switchToResultView}
              />
            </div>
          </div>
        </form>
      </div>
    );
  }
}
