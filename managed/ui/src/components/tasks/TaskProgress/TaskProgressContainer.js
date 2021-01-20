// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';

import {
  fetchTaskProgress,
  fetchTaskProgressResponse,
  resetTaskProgress
} from '../../../actions/tasks';
import { TaskProgress } from '../../tasks';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchTaskProgress: (taskUUID) => {
      dispatch(fetchTaskProgress(taskUUID)).then((response) => {
        dispatch(fetchTaskProgressResponse(response.payload));
      });
    },
    resetTaskProgress: () => {
      dispatch(resetTaskProgress());
    }
  };
};

function mapStateToProps(state, ownProps) {
  return {
    taskProgressData: state.tasks.taskProgressData
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(TaskProgress);
