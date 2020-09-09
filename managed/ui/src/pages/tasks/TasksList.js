// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { TasksListContainer } from '../../components/tasks';

export default class TasksList extends Component {
  render() {
    return (
      <div>
        <TasksListContainer/>
      </div>
    );
  }
}
