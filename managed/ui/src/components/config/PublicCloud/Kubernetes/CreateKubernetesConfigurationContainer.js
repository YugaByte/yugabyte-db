// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import CreateKubernetesConfiguration from './CreateKubernetesConfiguration';

import {
  createProviderResponse,
  createMultiRegionKubernetesProvider,
  fetchCloudMetadata
} from '../../../../actions/cloud';
import { openDialog, closeDialog } from '../../../../actions/modal';

const mapStateToProps = (state) => {
  return {
    modal: state.modal,
    customer: state.customer,
    providers: state.cloud.providers
  };
};

const mapDispatchToProps = (dispatch) => {
  return {
    createKubernetesProvider: (providerName, providerConfig, regionData) => {
      dispatch(createMultiRegionKubernetesProvider(providerName, providerConfig, regionData)).then(
        (response) => {
          dispatch(createProviderResponse(response.payload));
          if (response.payload.status === 200) {
            dispatch(fetchCloudMetadata());
          }
        }
      );
    },

    showModal: (modalName) => {
      dispatch(openDialog(modalName));
    },
    closeModal: () => {
      dispatch(closeDialog());
    }
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(CreateKubernetesConfiguration);
