// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';

import UniverseTable from './UniverseTable';
import { fetchUniverseMetadata, resetUniverseTasks } from '../../../actions/universe';
import {
  fetchCustomerTasks,
  fetchCustomerTasksSuccess,
  fetchCustomerTasksFailure
} from '../../../actions/tasks';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchUniverseMetadata: () => {
      dispatch(fetchUniverseMetadata());
    },

    fetchUniverseTasks: () => {
      dispatch(fetchCustomerTasks()).then((response) => {
        if (!response.error) {
          dispatch(fetchCustomerTasksSuccess(response.payload));
        } else {
          dispatch(fetchCustomerTasksFailure(response.payload));
        }
      });
    },
    resetUniverseTasks: () => {
      dispatch(resetUniverseTasks());
    }
  };
};

function mapStateToProps(state) {
  return {
    universe: state.universe,
    customer: state.customer,
    graph: state.graph,
    tasks: state.tasks,
    providers: state.cloud.providers
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(UniverseTable);
