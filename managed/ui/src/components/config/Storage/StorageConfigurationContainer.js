// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { reduxForm } from 'redux-form';
import { StorageConfiguration } from '../../config';
import {
  addCustomerConfig,
  addCustomerConfigResponse,
  fetchCustomerConfigs,
  fetchCustomerConfigsResponse,
  deleteCustomerConfig,
  deleteCustomerConfigResponse
} from '../../../actions/customers';
import { openDialog, closeDialog } from '../../../actions/modal';

const mapStateToProps = (state) => {
  return {
    addConfig: state.customer.addConfig,
    customerConfigs: state.customer.configs,
    visibleModal: state.modal.visibleModal,
    deleteConfig: state.customer.deleteConfig
  };
};

const mapDispatchToProps = (dispatch) => {
  return {
    addCustomerConfig: (config) => {
      return dispatch(addCustomerConfig(config)).then((response) => {
        return dispatch(addCustomerConfigResponse(response.payload));
      });
    },

    deleteCustomerConfig: (configUUID) => {
      return dispatch(deleteCustomerConfig(configUUID)).then((response) => {
        return dispatch(deleteCustomerConfigResponse(response.payload));
      });
    },

    fetchCustomerConfigs: () => {
      dispatch(fetchCustomerConfigs()).then((response) => {
        dispatch(fetchCustomerConfigsResponse(response.payload));
      });
    },

    showDeleteStorageConfig: (configName) => {
      dispatch(openDialog('delete' + configName + 'StorageConfig'));
    },

    hideDeleteStorageConfig: () => {
      dispatch(closeDialog());
    }
  };
};

const storageConfigForm = reduxForm({
  form: 'storageConfigForm'
});

export default connect(
  mapStateToProps,
  mapDispatchToProps
)(storageConfigForm(StorageConfiguration));
