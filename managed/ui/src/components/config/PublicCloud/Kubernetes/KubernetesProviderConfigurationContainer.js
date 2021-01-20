// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { KubernetesProviderConfiguration } from '../../../config';
import {
  deleteProvider,
  deleteProviderFailure,
  deleteProviderSuccess,
  fetchCloudMetadata
} from '../../../../actions/cloud';
import { openDialog, closeDialog } from '../../../../actions/modal';

const mapDispatchToProps = (dispatch) => {
  return {
    deleteProviderConfig: (providerUUID) => {
      dispatch(deleteProvider(providerUUID)).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(deleteProviderFailure(response.payload));
        } else {
          dispatch(deleteProviderSuccess(response.payload));
          dispatch(fetchCloudMetadata());
        }
      });
    },
    showDeleteConfirmationModal: () => {
      dispatch(openDialog('confirmDeleteProviderModal'));
    },
    closeModal: () => {
      dispatch(closeDialog());
    }
  };
};

const mapStateToProps = (state) => {
  return {
    universeList: state.universe.universeList,
    providers: state.cloud.providers,
    regions: state.cloud.supportedRegionList,
    modal: state.modal
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(KubernetesProviderConfiguration);
