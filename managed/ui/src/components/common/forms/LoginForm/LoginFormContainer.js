// Copyright (c) YugaByte, Inc.

import LoginForm from './LoginForm';
import {login, loginResponse, resetCustomerError}
  from '../../../../actions/customers';
import { connect } from 'react-redux';

const mapDispatchToProps = (dispatch) => {
  return {
    loginCustomer: (formValues) => {
      dispatch(login(formValues)).then((response) => {
        if (response.payload.status === 200) {
          const { authToken, customerUUID, userUUID } = response.payload.data;
          localStorage.setItem('authToken', authToken);
          localStorage.setItem('customerId', customerUUID);
          localStorage.setItem('userId', userUUID);
        }
        dispatch(loginResponse(response.payload));
      });
    },
    resetCustomerError: () => dispatch(resetCustomerError()),
  };
};

function mapStateToProps(state) {
  return {
    customer: state.customer
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(LoginForm);
