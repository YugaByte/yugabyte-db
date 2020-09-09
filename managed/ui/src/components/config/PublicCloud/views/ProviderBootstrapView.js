// Copyright (c) YugaByte, Inc.

import React, {Component} from 'react';
import { reduxForm } from 'redux-form';
import { Row, Col } from 'react-bootstrap';
import { YBButton } from '../../../common/forms/fields';
import { YBConfirmModal } from '../../../modals';
import {TaskProgressContainer} from '../../../tasks';
import { PROVIDER_TYPES } from '../../../../config';

class ProviderBootstrapView extends Component {
  deleteProviderConfig = provider => {
    this.props.deleteProviderConfig(provider.uuid);
  };

  showDeleteProviderModal = () => {
    const {providerType, showDeleteProviderModal} = this.props;
    switch (providerType) {
      case 'aws': showDeleteProviderModal('deleteAWSProvider'); break;
      case 'gcp': showDeleteProviderModal('deleteGCPProvider'); break;
      case 'azu': showDeleteProviderModal('deleteAzureProvider'); break;
      default: break;
    }
  };

  render() {
    const {currentProvider, handleSubmit, providerType, currentModal, visibleModal} = this.props;
    const providerMeta = PROVIDER_TYPES.find(item => item.code === providerType);
    // Delete Button is never disabled in Bootstrap view for now, in future add disable if task is pending
    const deleteButtonDisabled = false;
    const deleteButtonClassName = "btn btn-default manage-provider-btn delete-btn";
    return (
      <div>
        <Row className="config-section-header">
          <Col md={12}>
            <span className="create-provider-label-text">Creating Provider</span>
            <span className="pull-right" title={`Delete ${providerType.toUpperCase()} Config`}>
              <YBButton btnText="Delete Configuration" disabled={deleteButtonDisabled}
                        btnClass={deleteButtonClassName} onClick={this.showDeleteProviderModal}/>
              <YBConfirmModal name="deleteProvider" title={"Confirm Delete"}
                              onConfirm={handleSubmit(this.deleteProviderConfig.bind(this, currentProvider))}
                              currentModal={currentModal} visibleModal={visibleModal}
                              hideConfirmModal={this.props.hideDeleteProviderModal}>
                Are you sure you want to delete this {providerMeta.label} configuration?
              </YBConfirmModal>
            </span>
          </Col>
        </Row>
        <Row>
          <Col md={12}>
            <TaskProgressContainer type={"StepBar"} taskUUIDs={this.props.taskUUIDs} onTaskSuccess={this.props.reloadCloudMetadata}/>
          </Col>
        </Row>
      </div>
    );
  }
}

export default reduxForm({
  form: 'deleteProvider'
})(ProviderBootstrapView);
