// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import './stylesheets/StepProgressBar.scss';

export default class StepProgressBar extends Component {
  isFailedIndex = (taskDetails) => {
    return taskDetails.findIndex((element) => {
      return element.state === 'Failure';
    });
  };

  isRunningIndex = (taskDetails) => {
    return taskDetails.findIndex((element) => {
      return element.state === 'Running';
    });
  };

  normalizeTasks = (taskDetails) => {
    const taskDetailsNormalized = [
      ...taskDetails,
      {
        description: 'Universe created',
        state: 'Unknown',
        title: 'Done'
      }
    ];
    if (this.isFailedIndex(taskDetailsNormalized) > -1) {
      for (let i = 0; i < this.isFailedIndex(taskDetailsNormalized); i++) {
        taskDetailsNormalized[i].class = 'to-be-failed';
      }
    } else if (this.isRunningIndex(taskDetailsNormalized) > -1) {
      for (let i = 0; i < this.isRunningIndex(taskDetailsNormalized); i++) {
        taskDetailsNormalized[i].class = 'to-be-succeed';
      }
    } else {
      taskDetailsNormalized.forEachclass = 'finished';
      taskDetailsNormalized[taskDetailsNormalized.length - 1].class = 'finished';
    }
    return taskDetailsNormalized;
  };

  render() {
    const {
      details: { taskDetails }
    } = this.props.progressData;
    let taskClassName = '';
    const getTaskClass = function (type) {
      if (type === 'Initializing' || type === 'Unknown') {
        return 'pending';
      } else if (type === 'Success') {
        return 'finished';
      } else if (type === 'Running') {
        return 'running';
      } else if (type === 'Failure') {
        return 'failed';
      }
      return null;
    };

    const taskDetailsNormalized = this.normalizeTasks(taskDetails);

    const tasksTotal = taskDetailsNormalized.length;
    const taskIndex = taskDetailsNormalized.findIndex((element) => {
      return element.state === 'Running' || element.state === 'Failure';
    });

    const progressbarClass =
      this.isFailedIndex(taskDetailsNormalized) > -1
        ? 'failed'
        : this.isRunningIndex(taskDetailsNormalized) > -1
          ? 'running'
          : 'finished';
    const barWidth =
      taskIndex === -1
        ? '100%'
        : (100 * (taskIndex + (this.isFailedIndex(taskDetailsNormalized) > -1 ? 0 : 0.5))) /
            (tasksTotal - 1) +
          '%';

    const listLabels = taskDetailsNormalized.map(function (item, idx) {
      taskClassName = getTaskClass(item.state);
      return (
        <li key={idx} className={taskClassName + ' ' + item.class}>
          <span>{item.title}</span>
        </li>
      );
    }, this);
    return (
      <ul className="progressbar">
        <div
          className={
            'progressbar-bar ' +
            progressbarClass +
            ' ' +
            (taskIndex > -1 ? getTaskClass(taskDetailsNormalized[taskIndex].state) : '')
          }
          style={{ width: barWidth }}
        ></div>
        {listLabels}
      </ul>
    );
  }
}
