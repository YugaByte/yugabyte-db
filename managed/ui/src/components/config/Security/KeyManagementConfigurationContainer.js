// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { KeyManagementConfiguration } from '../../config';
import { fetchCustomerConfigs, fetchCustomerConfigsResponse } from '../../../actions/customers';
import {
  createKMSProviderConfig,
  createKMSProviderConfigResponse,
  fetchAuthConfigList,
  fetchAuthConfigListResponse,
  deleteKMSProviderConfig,
  deleteKMSProviderConfigResponse
} from '../../../actions/cloud';

const mapStateToProps = (state) => {
  return {
    customerConfigs: state.customer.configs,
    configList: state.cloud.authConfig,
    visibleModal: state.modal.visibleModal,
    deleteConfig: state.customer.deleteConfig
  };
};

const mapDispatchToProps = (dispatch) => {
  return {
    fetchCustomerConfigs: () => {
      return dispatch(fetchCustomerConfigs()).then((response) =>
        dispatch(fetchCustomerConfigsResponse(response.payload))
      );
    },

    fetchKMSConfigList: () => {
      return dispatch(fetchAuthConfigList()).then((response) =>
        dispatch(fetchAuthConfigListResponse(response.payload))
      );
    },

    setKMSConfig: (provider, body) => {
      return dispatch(createKMSProviderConfig(provider, body))
        .then((response) => {
          return dispatch(createKMSProviderConfigResponse(response.payload));
        })
        .catch((err) => console.err('Error submitting KMS configuration: ', err));
    },

    deleteKMSConfig: (configUUID) => {
      dispatch(deleteKMSProviderConfig(configUUID))
        .then((response) => {
          if (response.payload.status === 200) {
            return dispatch(deleteKMSProviderConfigResponse(configUUID));
          }
          console.warn('Warning: Deleting configuration returned unsuccessful response.');
        })
        .catch((err) => console.error(err));
    }
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(KeyManagementConfiguration);
