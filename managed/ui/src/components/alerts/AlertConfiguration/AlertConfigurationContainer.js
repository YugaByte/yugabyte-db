// Copyright (c) YugaByte, Inc.
//
// Author: Nishant Sharma(nissharma@deloitte.com)
//

import { connect } from 'react-redux';
import { toast } from 'react-toastify';
import { change } from 'redux-form';
import {
  alertConfigs,
  alertDestionations,
  createAlertConfig,
  createAlertConfigResponse,
  createAlertDestination,
  createAlertDestinationResponse,
  createAlertReceiver,
  createAlertReceiverResponse,
  deleteAlertConfig,
  deleteAlertDestination,
  getAlertReceivers,
  getTargetMetrics,
  setInitialValues,
  updateAlertConfig,
  updateAlertConfigResponse,
  updateAlertDestination,
  updateAlertDestinationResponse,
  updateProfile,
  updateProfileFailure,
  updateProfileSuccess
} from '../../../actions/customers';
import { closeDialog, openDialog } from '../../../actions/modal';
import { fetchUniverseList, fetchUniverseListResponse } from '../../../actions/universe';
import { AlertConfiguration } from './AlertConfiguration';

const mapStateToProps = (state) => {
  return {
    customer: state.customer.currentCustomer,
    users: state.customer.users.data,
    apiToken: state.customer.apiToken,
    customerProfile: state.customer ? state.customer.profile : null,
    modal: state.modal,
    initialValues: state.customer.setInitialVal,
    universes: state.universe.universeList
  };
};

const mapDispatchToProps = (dispatch) => {
  return {
    alertConfigs: (payload) => {
      return dispatch(alertConfigs(payload)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
          return;
        }
        return response.payload.data;
      });
    },
    alertDestionations: () => {
      return dispatch(alertDestionations()).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
          return;
        }
        return response.payload.data;
      });
    },
    getTargetMetrics: (payload) => {
      return dispatch(getTargetMetrics(payload)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
          return;
        }
        return response.payload.data;
      });
    },
    setInitialValues: (initialValues) => {
      return dispatch(setInitialValues(initialValues));
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
    createAlertChannel: (payload) => {
      return dispatch(createAlertReceiver(payload)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
        } else {
          toast.success('Successfully created the channel');
        }
        return dispatch(createAlertReceiverResponse(response.payload));
      });
    },
    getAlertReceivers: () => {
      return dispatch(getAlertReceivers()).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
          return;
        }
        return response.payload.data;
      });
    },
    createAlertDestination: (payload) => {
      return dispatch(createAlertDestination(payload)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
        } else {
          toast.success('Successfully added the destination');
        }
        return dispatch(createAlertDestinationResponse(response.payload));
      });
    },
    createAlertConfig: (payload) => {
      return dispatch(createAlertConfig(payload)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
        } else {
          toast.success('Successfully added the destination');
        }
        return dispatch(createAlertConfigResponse(response.payload));
      });
    },
    updateAlertConfig: (payload, uuid) => {
      return dispatch(updateAlertConfig(payload, uuid)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
        } else {
          toast.success('Successfully updated the config');
        }
        return dispatch(updateAlertConfigResponse(response.payload));
      });
    },
    updateAlertDestination: (payload, uuid) => {
      return dispatch(updateAlertDestination(payload, uuid)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
        } else {
          toast.success('Successfully updated the destination');
        }
        return dispatch(updateAlertDestinationResponse(response.payload));
      });
    },
    deleteAlertDestination: (uuid) => {
      return dispatch(deleteAlertDestination(uuid)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
          return;
        }
        return response.payload.data;
      });
    },
    deleteAlertConfig: (uuid) => {
      return dispatch(deleteAlertConfig(uuid)).then((response) => {
        if (response.error) {
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(errorMessage);
          return;
        }
        return response.payload.data;
      });
    },
    fetchUniverseList: () => {
      return new Promise((resolve) => {
        dispatch(fetchUniverseList()).then((response) => {
          dispatch(fetchUniverseListResponse(response.payload));
          resolve(response.payload.data);
        });
      });
    },
    closeModal: () => {
      dispatch(closeDialog());
    },
    showAddChannelModal: () => {
      dispatch(openDialog('alertDestinationForm'));
    },
    showDeleteModal: (name) => {
      dispatch(openDialog(name));
    },
    showDetailsModal: () => {
      dispatch(openDialog('alertDestinationDetailsModal'));
    },
    updateField: (form, field, newValue) => dispatch(change(form, field, newValue))
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(AlertConfiguration);
