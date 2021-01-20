// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import Dashboard from './Dashboard';
import { fetchUniverseList, fetchUniverseListResponse } from '../../actions/universe';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchUniverseList: () => {
      dispatch(fetchUniverseList()).then((response) => {
        dispatch(fetchUniverseListResponse(response.payload));
      });
    }
  };
};

const mapStateToProps = (state) => {
  return {
    customer: state.customer,
    universe: state.universe,
    cloud: state.cloud
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(Dashboard);
