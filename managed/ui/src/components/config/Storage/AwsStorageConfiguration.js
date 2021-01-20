// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { YBButton, YBToggle, YBTextInputWithLabel } from '../../common/forms/fields';
import { Field } from 'redux-form';
import { YBConfirmModal } from '../../modals';
import { isDefinedNotNull, isEmptyObject } from '../../../utils/ObjectUtils';
import YBInfoTip from '../../common/descriptors/YBInfoTip';

const required = value => value ? undefined : 'This field is required.';

class AwsStorageConfiguration extends Component {
  state = {
    iamRoleEnabled: false
  };

  iamInstanceToggle = (event) => {
    this.setState({ iamRoleEnabled: event.target.checked });
  };

  render() {
    const {
      customerConfigs,
      submitting,
      addConfig: { loading },
      deleteStorageConfig,
      showDeleteStorageConfig
    } = this.props;
    const { iamRoleEnabled } = this.state;
    const s3Config = customerConfigs.data.find((config) => config.name === 'S3');
    const config = s3Config ? s3Config.data : {};
    const allowKeyEdits = !isEmptyObject(s3Config) || iamRoleEnabled;
    return (
      <Row className="config-section-header" key={'s3'}>
        <Col lg={9}>
          <Row className="config-provider-row" key={'s3-iam-instance-profile'}>
            <Col lg={2}>
              <div className="form-item-custom-label">IAM Role</div>
            </Col>
            <Col lg={9}>
              {!isEmptyObject(s3Config) ? (
                <Field
                  name="IAM_INSTANCE_PROFILE"
                  component={YBToggle}
                  input={{
                    name: 'IAM_INSTANCE_PROFILE',
                    value: config['IAM_INSTANCE_PROFILE']
                  }}
                  subLabel="Whether to use instance's IAM role for S3 backup."
                  isReadOnly
                />
              ) : (
                <Field
                  name="IAM_INSTANCE_PROFILE"
                  component={YBToggle}
                  onToggle={this.iamInstanceToggle}
                  subLabel="Whether to use instance's IAM role for S3 backup."
                />
              )}
            </Col>
          </Row>
          <Row className="config-provider-row" key={'s3-aws-access-key-id'}>
            <Col lg={2}>
              <div className="form-item-custom-label">Access Key</div>
            </Col>
            <Col lg={9}>
              {allowKeyEdits ? (
                <Field
                  name="AWS_ACCESS_KEY_ID"
                  placeHolder="AWS Access Key"
                  input={{
                    value: config['AWS_ACCESS_KEY_ID'] || '',
                    disabled: allowKeyEdits
                  }}
                  component={YBTextInputWithLabel}
                />
              ) : (
                <Field
                  name="AWS_ACCESS_KEY_ID"
                  placeHolder="AWS Access Key"
                  component={YBTextInputWithLabel}
                  validate={required}
                />
              )}
            </Col>
          </Row>
          <Row className="config-provider-row" key={'s3-aws-secret-access-key'}>
            <Col lg={2}>
              <div className="form-item-custom-label">Access Secret</div>
            </Col>
            <Col lg={9}>
              {allowKeyEdits ? (
                <Field
                  name="AWS_SECRET_ACCESS_KEY"
                  placeHolder="AWS Access Secret"
                  input={{
                    value: config['AWS_SECRET_ACCESS_KEY'] || '',
                    disabled: allowKeyEdits
                  }}
                  component={YBTextInputWithLabel}
                />
              ) : (
                <Field
                  name="AWS_SECRET_ACCESS_KEY"
                  placeHolder="AWS Access Secret"
                  component={YBTextInputWithLabel}
                  validate={required}
                />
              )}
            </Col>
          </Row>
          <Row className="config-provider-row" key={'s3-backup-location'}>
            <Col lg={2}>
              <div className="form-item-custom-label">S3 Bucket</div>
            </Col>
            <Col lg={9}>
              {!isEmptyObject(s3Config) ? (
                <Field
                  name="BACKUP_LOCATION"
                  placeHolder="S3 Bucket"
                  input={{
                    value: config['BACKUP_LOCATION'],
                    disabled: !isEmptyObject(s3Config)
                  }}
                  component={YBTextInputWithLabel}
                />
              ) : (
                <Field
                  name="BACKUP_LOCATION"
                  placeHolder="S3 Bucket"
                  component={YBTextInputWithLabel}
                  validate={required}
                />
              )}
            </Col>
          </Row>
          <Row className="config-provider-row" key={'s3-backup-host-base'}>
            <Col lg={2}>
              <div className="form-item-custom-label">S3 Bucket Host Base</div>
            </Col>
            <Col lg={9}>
              {!isEmptyObject(s3Config) ? (
                <Field
                  name="AWS_HOST_BASE"
                  placeHolder="s3.amazonaws.com"
                  input={{
                    value: config['AWS_HOST_BASE'],
                    disabled: !isEmptyObject(s3Config)
                  }}
                  component={YBTextInputWithLabel}
                />
              ) : (
                <Field
                  name="AWS_HOST_BASE"
                  placeHolder="s3.amazonaws.com"
                  component={YBTextInputWithLabel}
                />
              )}
            </Col>
            <Col lg={1} className="config-zone-tooltip">
              <YBInfoTip
                title="S3 Host"
                content="Host of S3 bucket. Defaults to s3.amazonaws.com"
              />
            </Col>
          </Row>
        </Col>
        {!isEmptyObject(s3Config) && (
          <Col lg={3}>
            <div className="action-bar">
              {s3Config.inUse && (
                <YBInfoTip content={"Storage configuration is in use and cannot be deleted until associated resources are removed."}
                  placement="top"
                >
                  <span className="disable-delete fa-stack fa-2x">
                    <i className="fa fa-trash-o fa-stack-1x"></i>
                    <i className="fa fa-ban fa-stack-2x"></i>
                  </span>
                </YBInfoTip>
              )}
              <YBButton
                btnText={'Delete Configuration'}
                disabled={s3Config.inUse || submitting || loading}
                btnClass={'btn btn-default'}
                onClick={
                  !isEmptyObject(s3Config)
                    ? showDeleteStorageConfig.bind(this, s3Config.name)
                    : null
                }
              />
              {isDefinedNotNull(config) && (
                <YBConfirmModal
                  name="delete-storage-config"
                  title={'Confirm Delete'}
                  onConfirm={() => deleteStorageConfig(s3Config.configUUID)}
                  currentModal={'delete' + s3Config.name + 'StorageConfig'}
                  visibleModal={this.props.visibleModal}
                  hideConfirmModal={this.props.hideDeleteStorageConfig}
                >
                  Are you sure you want to delete {config.name} Storage Configuration?
                </YBConfirmModal>
              )}
            </div>
          </Col>
        )}
      </Row>
    );
  }
}

export default AwsStorageConfiguration;
