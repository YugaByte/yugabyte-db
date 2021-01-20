// Copyright (c) YugaByte, Inc.

import React, { Component, ReactNode } from 'react';
import { Row, Col } from 'react-bootstrap';

import { isNonEmptyObject } from '../../../utils/ObjectUtils';
import { YBModalForm } from '../../common/forms';

export enum ReleaseStateEnum {
  ACTIVE = 'ACTIVE',
  DISABLED = 'DISABLED',
  DELETED = 'DELETED'
};

interface UpdateReleaseProps {
  visible: boolean,
  releaseInfo: {
    version: string,
  },  
  actionType: ReleaseStateEnum,
  onHide(): void,
  updateYugaByteRelease(value: string, state: object): void,
  onModalSubmit(): void
}

export default class UpdateRelease extends Component<UpdateReleaseProps> {
  updateRelease = () => {
    const { releaseInfo, updateYugaByteRelease, onModalSubmit, onHide, actionType } = this.props;
    updateYugaByteRelease(releaseInfo.version, {
      state: actionType
    });
    onHide();
    onModalSubmit();
  };

  render() {
    const { visible, onHide, releaseInfo, actionType } = this.props;
    if (!isNonEmptyObject(releaseInfo)) {
      return <div />;
    }
    let modalTitle: (string | ReactNode) = '';
    switch (actionType) {
      case ReleaseStateEnum.DISABLED:
        modalTitle = <div>Disable Release <code>{releaseInfo.version}</code></div>;
        break;
      case ReleaseStateEnum.DELETED:
        modalTitle = <div>Delete Release <code>{releaseInfo.version}</code></div>;
        break;
      case ReleaseStateEnum.ACTIVE:
        modalTitle = <div>Activate Release <code>{releaseInfo.version}</code></div>;
        break;
      default:
        modalTitle = 'Update Release ' + releaseInfo.version;
    }
    return (
      <div className="universe-apps-modal">
        <YBModalForm
          title={modalTitle}
          visible={visible}
          onHide={onHide}
          showCancelButton={true}
          cancelLabel={'Cancel'}
          submitLabel={'Yes'}
          className="import-release-modal"
          onFormSubmit={this.updateRelease}
        >
          <Row>
            <Col lg={12}>Are you sure you want to perform this action?</Col>
          </Row>
        </YBModalForm>
      </div>
    );
  }
}
