// Copyright YugaByte Inc.

import { connect } from 'react-redux';
import { CustomerProfile } from '../profile';
import {
  updateProfile,
  updateProfileSuccess,
  updateProfileFailure,
  getApiToken,
  getApiTokenResponse,
  getApiTokenLoading,
  getCustomerUsers,
  getCustomerUsersSuccess,
  getCustomerUsersFailure,
  updatePassword,
  updatePasswordSuccess,
  updatePasswordFailure
} from '../../actions/customers';

const mapDispatchToProps = (dispatch) => {
  return {
    getCustomerUsers: () => {
      dispatch(getCustomerUsers()).then((response) => {
        try {
          if (response.payload.status !== 200) {
            dispatch(getCustomerUsersFailure(response.payload));
          } else {
            dispatch(getCustomerUsersSuccess(response.payload));
          }
        } catch (error) {
          console.error('Error while fetching customer users');
        }
      });
    },
    updateCustomerDetails: (values) => {
      dispatch(updateProfile(values)).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(updateProfileFailure(response.payload));
        } else {
          dispatch(updateProfileSuccess(response.payload));
        }
      });
    },
    changeUserPassword: (userUUID, values) => {
      dispatch(updatePassword(userUUID, values)).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(updatePasswordFailure(response.payload));
        } else {
          dispatch(updatePasswordSuccess(response.payload));
        }
      });
    },
    refreshApiToken: (authToken) => {
      dispatch(getApiTokenLoading());
      dispatch(getApiToken(authToken)).then((response) => {
        if (response.payload.status === 200) {
          response.payload.data = response.payload.data.apiToken;
        }
        dispatch(getApiTokenResponse(response.payload));
      });
    }
  };
};

function mapStateToProps(state) {
  return {
    customer: state.customer.currentCustomer,
    users: state.customer.users.data,
    apiToken: state.customer.apiToken,
    customerProfile: state.customer ? state.customer.profile : null
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(CustomerProfile);
