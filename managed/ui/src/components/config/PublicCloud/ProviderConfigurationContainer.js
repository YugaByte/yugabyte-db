// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import ProviderConfiguration from './ProviderConfiguration';
import { reset } from 'redux-form';
import { createProvider, createProviderResponse, createRegion, createRegionResponse,
  createAccessKey, createAccessKeyResponse, initializeProvider, initializeProviderSuccess,
  initializeProviderFailure, deleteProvider, deleteProviderSuccess, deleteProviderFailure,
  resetProviderBootstrap, fetchCloudMetadata, bootstrapProvider, bootstrapProviderResponse } from '../../../actions/cloud';
import { openDialog, closeDialog } from '../../../actions/modal';
import {fetchTaskProgress, fetchTaskProgressResponse,fetchCustomerTasks , fetchCustomerTasksFailure, fetchCustomerTasksSuccess }
  from '../../../actions/tasks';
import { fetchHostInfo, fetchHostInfoSuccess, fetchHostInfoFailure } from '../../../actions/customers';

const mapDispatchToProps = (dispatch) => {
  return {
    createAWSProvider: (name, config, regionFormVals) => {
      Object.keys(config).forEach((key) => { if (typeof config[key] === 'string' || config[key] instanceof String) config[key] = config[key].trim(); });
      Object.keys(regionFormVals).forEach((key) => { if (typeof regionFormVals[key] === 'string' || regionFormVals[key] instanceof String) regionFormVals[key] = regionFormVals[key].trim(); });
      dispatch(createProvider("aws", name.trim(), config)).then((response) => {
        dispatch(createProviderResponse(response.payload));
        if (response.payload.status === 200) {
          dispatch(fetchCloudMetadata());
          const providerUUID = response.payload.data.uuid;
          dispatch(bootstrapProvider(providerUUID, regionFormVals)).then((boostrapResponse) => {
            dispatch(bootstrapProviderResponse(boostrapResponse.payload));
          });
        }
      });
    },

    createGCPProvider: (providerName, providerConfig, perRegionMetadata) => {
      Object.keys(providerConfig).forEach((key) => { if (typeof providerConfig[key] === 'string' || providerConfig[key] instanceof String) providerConfig[key] = providerConfig[key].trim(); });
      dispatch(createProvider("gcp", providerName.trim(), providerConfig)).then((response) => {
        dispatch(createProviderResponse(response.payload));
        if (response.payload.status === 200) {
          dispatch(fetchCloudMetadata());
          const providerUUID = response.payload.data.uuid;
          const hostNetwork = providerConfig["network"];
          const params = {
            "hostVpcId": hostNetwork,
            "destVpcId": hostNetwork,
            "airGapInstall": providerConfig["airGapInstall"],
            "sshPort": providerConfig["sshPort"],
            "perRegionMetadata": perRegionMetadata
          };
          dispatch(bootstrapProvider(providerUUID, params)).then((boostrapResponse) => {
            dispatch(bootstrapProviderResponse(boostrapResponse.payload));
          });
        }
      });
    },

    createAzureProvider: (name, config, regionFormVals) => {
      Object.keys(config).forEach((key) => { if (typeof config[key] === 'string' || config[key] instanceof String) config[key] = config[key].trim(); });
      Object.keys(regionFormVals).forEach((key) => { if (typeof regionFormVals[key] === 'string' || regionFormVals[key] instanceof String) regionFormVals[key] = regionFormVals[key].trim(); });
      dispatch(createProvider('azu', name.trim(), config)).then((response) => {
        dispatch(createProviderResponse(response.payload));
        if (response.payload.status === 200) {
          dispatch(fetchCloudMetadata());
          const providerUUID = response.payload.data.uuid;
          dispatch(bootstrapProvider(providerUUID, regionFormVals)).then((boostrapResponse) => {
            dispatch(bootstrapProviderResponse(boostrapResponse.payload));
          });
        }
      });
    },

    createRegion: (providerUUID, formVals) => {
      dispatch(createRegion(providerUUID, formVals)).then((response) => {
        dispatch(createRegionResponse(response.payload));
      });
    },

    createAccessKey: (providerUUID, regionUUID, accessKeyCode) => {
      const keyInfo = {'code': accessKeyCode};
      dispatch(createAccessKey(providerUUID, regionUUID, keyInfo)).then((response) => {
        dispatch(createAccessKeyResponse(response.payload));
      });
    },

    initializeProvider: (providerUUID) => {
      dispatch(initializeProvider(providerUUID)).then((response) => {
        if(response.payload.status !== 200) {
          dispatch(initializeProviderFailure(response.payload));
        } else {
          dispatch(initializeProviderSuccess(response.payload));
        }
      });
    },

    deleteProviderConfig: (providerUUID) => {
      dispatch(deleteProvider(providerUUID)).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(deleteProviderFailure(response.payload));
        } else {
          dispatch(deleteProviderSuccess(response.payload));
          dispatch(fetchCloudMetadata());
          dispatch(reset('awsConfigForm'));
          // TODO: maybe need to reset azure form as well
        }
      });
    },

    resetProviderBootstrap: () => {
      dispatch(resetProviderBootstrap());
    },

    // Valid Provider Types are:
    // deleteGCPProvider, deleteAWSProvider, deleteAzureProvider
    showDeleteProviderModal: (providerType) => {
      dispatch(openDialog(providerType));
    },

    hideDeleteProviderModal: () => {
      dispatch(closeDialog());
    },

    reloadCloudMetadata: () => {
      dispatch(fetchCloudMetadata());
    },

    getCurrentTaskData: (taskUUID) => {
      dispatch(fetchTaskProgress(taskUUID)).then((response) => {
        dispatch(fetchTaskProgressResponse(response.payload));
      });
    },

    fetchCustomerTasksList: () => {
      dispatch(fetchCustomerTasks()).then((response) => {
        if (response.payload.status === 200) {
          dispatch(fetchCustomerTasksSuccess(response.payload));
        } else {
          dispatch(fetchCustomerTasksFailure(response.payload));
        }
      });
    },
    showModal: (modalName) => {
      dispatch(openDialog(modalName));
    },
    closeModal: () => {
      dispatch(closeDialog());
    },
    fetchHostInfo: () => {
      dispatch(fetchHostInfo()).then((response)=>{
        if (response.payload.status !== 200) {
          dispatch(fetchHostInfoFailure(response.payload));
        } else {
          dispatch(fetchHostInfoSuccess(response.payload));
        }
      });
    },
  };
};


const mapStateToProps = (state) => {
  return {
    configuredProviders: state.cloud.providers,
    configuredRegions: state.cloud.supportedRegionList,
    accessKeys: state.cloud.accessKeys,
    cloudBootstrap: state.cloud.bootstrap,
    universeList: state.universe.universeList,
    hostInfo: state.customer.hostInfo,
    modal: state.modal,
    cloud: state.cloud,
    tasks: state.tasks,
    featureFlags: state.customer.currentCustomer?.data?.features,
  };
};


export default connect(mapStateToProps, mapDispatchToProps)(ProviderConfiguration);
