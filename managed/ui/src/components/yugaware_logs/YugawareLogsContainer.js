// Copyright YugaByte Inc.

import { connect } from 'react-redux';
import { YugawareLogs } from '../yugaware_logs';
import { reduxForm } from 'redux-form';
import { getLogs, getLogsSuccess, getLogsFailure } from '../../actions/customers';

const mapDispatchToProps = (dispatch) => {
  return {
    getLogs: () => {
      return dispatch(getLogs()).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(getLogsFailure(response.payload));
          const payload = response.payload;
          const error = payload.data ? payload.data.error : payload;
          console.error(error);
        } else {
          dispatch(getLogsSuccess(response.payload));
        }
      });
    }
  };
};

function mapStateToProps(state) {
  return {
    currentCustomer: state.customer.currentCustomer,
    yugawareLogs: state.customer.yugaware_logs,
    logError: state.customer.yugawareLogError
  };
}

function validate(values) {
  return false;
}

const getYugawareLogs = reduxForm({
  form: 'getYugawareLogs',
  fields: [],
  validate
});

export default connect(mapStateToProps, mapDispatchToProps)(getYugawareLogs(YugawareLogs));
