// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import './UniverseStatus.scss';
import { ProgressBar } from 'react-bootstrap';
import { isNonEmptyObject, isNonEmptyArray, isDefinedNotNull } from '../../../utils/ObjectUtils';
import { YBLoadingCircleIcon } from '../../common/indicators';

export default class UniverseStatus extends Component {
  hasPendingTasksForUniverse = (customerTaskList) => {
    const {
      currentUniverse: { universeUUID }
    } = this.props;
    return isNonEmptyArray(customerTaskList)
      ? customerTaskList.some(function (taskItem) {
        return (
          taskItem.targetUUID === universeUUID &&
          (taskItem.status === 'Running' || taskItem.status === 'Initializing') &&
          Number(taskItem.percentComplete) !== 100 &&
          taskItem.target.toLowerCase() !== 'backup'
        );
      })
      : false;
  };

  componentDidUpdate(prevProps) {
    const {
      currentUniverse: { universeDetails },
      tasks: { customerTaskList },
      refreshUniverseData
    } = this.props;

    if (
      !universeDetails.updateInProgress &&
      !this.hasPendingTasksForUniverse(customerTaskList) &&
      this.hasPendingTasksForUniverse(prevProps.tasks.customerTaskList)
    ) {
      refreshUniverseData();
    }
  }

  render() {
    const {
      currentUniverse: { universeDetails, universeUUID },
      showLabelText,
      tasks: { customerTaskList }
    } = this.props;
    const updateInProgress = universeDetails.updateInProgress;
    const updateSucceeded = universeDetails.updateSucceeded;
    const errorString = universeDetails.errorString;
    let statusClassName = 'unknown';
    let statusText = '';
    const universePendingTask = isNonEmptyArray(customerTaskList)
      ? customerTaskList.find(function (taskItem) {
        return (
          taskItem.targetUUID === universeUUID &&
          (taskItem.status === 'Running' || taskItem.status === 'Initializing') &&
          Number(taskItem.percentComplete) !== 100 &&
          taskItem.target.toLowerCase() !== 'backup'
        );
      })
      : null;

    if (showLabelText) {
      statusText = 'Loading';
    }
    let statusDisplay = (
      <div className="status-pending-display-container">
        <YBLoadingCircleIcon size="small" />
        <span className="status-pending-name">{statusText}</span>
      </div>
    );
    if (!isDefinedNotNull(universePendingTask) && updateSucceeded) {
      statusClassName = 'good';
      if (showLabelText) {
        statusText = 'Ready';
      }
      statusDisplay = (
        <div>
          <i className="fa fa-check-circle" />
          {statusText && <span>{statusText}</span>}
        </div>
      );
    } else {
      if (updateInProgress && isNonEmptyObject(universePendingTask)) {
        if (showLabelText) {
          statusDisplay = (
            <div className="status-pending">
              <div className="status-pending-display-container">
                <YBLoadingCircleIcon size="small" />
                <span className="status-pending-name">
                  Pending&hellip;
                  {universePendingTask.percentComplete}%
                </span>
                <span className="status-pending-progress-container">
                  <ProgressBar
                    className={'pending-action-progress'}
                    now={universePendingTask.percentComplete}
                  />
                </span>
              </div>
            </div>
          );
        } else {
          statusDisplay = (
            <div className={'yb-orange'}>
              <YBLoadingCircleIcon size="small" />
            </div>
          );
        }
        statusClassName = 'pending';
      } else if (!updateInProgress && !updateSucceeded) {
        if (errorString === 'Preflight checks failed.') {
          statusClassName = 'warning';
          if (showLabelText) {
            statusText = 'Ready';
          }
        } else {
          statusClassName = 'bad';
          if (showLabelText) {
            statusText = 'Error';
          }
        }
        statusDisplay = (
          <div>
            <i className="fa fa-warning" />
            {statusText && <span>{statusText}</span>}
          </div>
        );
      }
    }

    return <div className={'universe-status ' + statusClassName}>{statusDisplay}</div>;
  }
}
