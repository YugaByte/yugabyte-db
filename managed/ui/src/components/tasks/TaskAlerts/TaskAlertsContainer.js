// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { TaskAlerts } from '../../tasks';
import {
  fetchCustomerTasks,
  fetchCustomerTasksSuccess,
  fetchCustomerTasksFailure,
  resetCustomerTasks
} from '../../../actions/tasks';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchCustomerTasks: () => {
      dispatch(fetchCustomerTasks()).then((response) => {
        if (!response.error) {
          dispatch(fetchCustomerTasksSuccess(response.payload));
        } else {
          dispatch(fetchCustomerTasksFailure(response.payload));
        }
      });
    },
    resetCustomerTasks: () => {
      dispatch(resetCustomerTasks());
    }
  };
};

function mapStateToProps(state, ownProps) {
  return {
    universe: state.universe,
    tasks: state.tasks
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(TaskAlerts);
