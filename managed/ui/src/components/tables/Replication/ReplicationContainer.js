// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import Replication from './Replication';
import { queryMetrics, queryMetricsSuccess, queryMetricsFailure, resetMetrics } from '../../../actions/graph';
import { getMasterLeader, getMasterLeaderResponse, resetMasterLeader } from '../../../actions/universe';
  
const mapDispatchToProps = (dispatch) => {
    return {
      queryMetrics: (queryParams, panelType) => {
        dispatch(queryMetrics(queryParams))
        .then((response) => {
          if (!response.error) {
            dispatch(queryMetricsSuccess(response.payload, panelType));
          } else {
            dispatch(queryMetricsFailure(response.payload, panelType));
          }
        });
      },
      resetMetrics: () => {
        dispatch(resetMetrics());
      },
      getMasterLeader: (uuid) => {
        dispatch(getMasterLeader(uuid)).then((response) => {
          dispatch(getMasterLeaderResponse(response.payload));
        });
      },
  
      resetMasterLeader: () => {
        dispatch(resetMasterLeader());
      },
    }
}

function mapStateToProps(state, ownProps) {
  return {
    currentUniverse: state.universe.currentUniverse,
    currentCustomer: state.customer.currentCustomer,
    graph: state.graph
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(Replication);
