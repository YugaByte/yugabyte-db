// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';

import NavBar from './NavBar';
import { logout, logoutSuccess, logoutFailure } from '../../../actions/customers';

const mapDispatchToProps = (dispatch) => {
  return {
    logoutProfile: () => {
      dispatch(logout()).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(logoutFailure(response.payload));
        } else {
          localStorage.removeItem('authToken');
          dispatch(logoutSuccess());
        }
      });
    }
  };
};

const mapStateToProps = (state) => {
  return {
    customer: state.customer
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(NavBar);
