import React, { Component } from 'react';
import { isEmptyArray, isNonEmptyArray, isNonEmptyObject } from '../../../../utils/ObjectUtils';
import { YBLoading } from '../../../common/indicators';
import { TaskListTable, TaskProgressContainer } from '../../../tasks';
import { TASK_SHORT_TIMEOUT } from '../../../tasks/constants';

export class UniverseTaskList extends Component {
  tasksForUniverse = () => {
    const {
      universe: {
        currentUniverse: {
          data: { universeUUID }
        }
      },
      tasks: { customerTaskList }
    } = this.props;
    const resultTasks = [];
    if (isNonEmptyArray(customerTaskList)) {
      customerTaskList.forEach((taskItem) => {
        if (taskItem.targetUUID === universeUUID) resultTasks.push(taskItem);
      });
    }
    return resultTasks;
  };

  render() {
    const {
      universe: { currentUniverse },
      tasks: { customerTaskList },
      isCommunityEdition
    } = this.props;
    const currentUniverseTasks = this.tasksForUniverse();
    let universeTaskUUIDs = [];
    const universeTaskHistoryArray = [];
    let universeTaskHistory = <span />;
    let currentTaskProgress = <span />;
    if (isEmptyArray(customerTaskList)) {
      universeTaskHistory = <YBLoading />;
      currentTaskProgress = <YBLoading />;
    }
    if (
      isNonEmptyArray(customerTaskList) &&
      isNonEmptyObject(currentUniverse.data) &&
      isNonEmptyArray(currentUniverseTasks)
    ) {
      universeTaskUUIDs = currentUniverseTasks
        .map(function (task) {
          universeTaskHistoryArray.push(task);
          return task.status !== 'Failure' && task.percentComplete !== 100 ? task.id : false;
        })
        .filter(Boolean);
    }
    if (isNonEmptyArray(universeTaskHistoryArray)) {
      const errorPlatformMessage = (
        <div className="oss-unavailable-warning">Only available on Yugabyte Platform.</div>
      );
      universeTaskHistory = (
        <TaskListTable
          taskList={universeTaskHistoryArray || []}
          isCommunityEdition={isCommunityEdition}
          overrideContent={errorPlatformMessage}
          title={'Task History'}
        />
      );
    }
    if (isNonEmptyArray(customerTaskList) && isNonEmptyArray(universeTaskUUIDs)) {
      currentTaskProgress = (
        <TaskProgressContainer
          taskUUIDs={universeTaskUUIDs}
          type="StepBar"
          timeoutInterval={TASK_SHORT_TIMEOUT}
        />
      );
    }
    return (
      <div className="universe-detail-content-container">
        {currentTaskProgress}
        {universeTaskHistory}
      </div>
    );
  }
}
