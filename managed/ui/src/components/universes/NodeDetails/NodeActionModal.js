// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { YBModal } from '../../common/forms/fields';
import PropTypes from 'prop-types';
import { browserHistory } from 'react-router';
import { NodeAction } from '../../universes';

export default class NodeActionModal extends Component {
  static propTypes = {
    nodeInfo: PropTypes.object.isRequired,
    actionType: PropTypes.string
  };

  performNodeAction = () => {
    const {
      universe: { currentUniverse },
      nodeInfo,
      actionType,
      performUniverseNodeAction,
      onHide
    } = this.props;
    const universeUUID = currentUniverse.data.universeUUID;
    performUniverseNodeAction(universeUUID, nodeInfo.name, actionType);
    onHide();
    browserHistory.push('/universes/' + universeUUID + '/nodes');
  };

  render() {
    const { visible, onHide, nodeInfo, actionType } = this.props;
    if (actionType === null || nodeInfo === null) {
      return <span />;
    }

    return (
      <div className="universe-apps-modal">
        <YBModal
          title={`Perform Node Action: ${NodeAction.getCaption(actionType)} `}
          visible={visible}
          onHide={onHide}
          showCancelButton={true}
          cancelLabel={'Cancel'}
          onFormSubmit={this.performNodeAction}
        >
          Are you sure you want to {actionType.toLowerCase()} {nodeInfo.name}?
        </YBModal>
      </div>
    );
  }
}
