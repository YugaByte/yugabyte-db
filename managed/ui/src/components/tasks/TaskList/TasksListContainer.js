// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { TasksList } from '../../tasks';
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

function mapStateToProps(state) {
  return {
    universe: state.universe,
    customer: state.customer,
    tasks: state.tasks
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(TasksList);
