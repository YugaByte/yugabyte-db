// Copyright (c) YugaByte, Inc.

import { reduxForm, formValueSelector } from 'redux-form';
import { connect } from 'react-redux';
import {
  fetchCustomerTasks,
  fetchCustomerTasksSuccess,
  fetchCustomerTasksFailure
} from '../../../actions/tasks';
import UniverseForm from './UniverseForm';
import {
  getInstanceTypeList,
  getRegionList,
  getRegionListResponse,
  getInstanceTypeListResponse,
  getNodeInstancesForProvider,
  getNodesInstancesForProviderResponse,
  fetchAuthConfigList,
  fetchAuthConfigListResponse
} from '../../../actions/cloud';
import { getTlsCertificates, getTlsCertificatesResponse } from '../../../actions/customers';
import {
  createUniverse,
  createUniverseResponse,
  editUniverse,
  editUniverseResponse,
  configureUniverseTemplate,
  configureUniverseTemplateResponse,
  configureUniverseTemplateSuccess,
  configureUniverseTemplateLoading,
  configureUniverseResources,
  configureUniverseResourcesResponse,
  checkIfUniverseExists,
  setPlacementStatus,
  resetUniverseConfiguration,
  fetchUniverseInfo,
  fetchUniverseInfoResponse,
  fetchUniverseMetadata,
  fetchUniverseTasks,
  fetchUniverseTasksResponse,
  addUniverseReadReplica,
  editUniverseReadReplica,
  addUniverseReadReplicaResponse,
  editUniverseReadReplicaResponse,
  closeUniverseDialog
} from '../../../actions/universe';

import { openDialog, closeDialog } from '../../../actions/modal';

import {
  isNonEmptyArray,
  isDefinedNotNull,
  isNonEmptyObject,
  isNonEmptyString,
  isEmptyObject
} from '../../../utils/ObjectUtils';
import { getClusterByType } from '../../../utils/UniverseUtils';

const mapDispatchToProps = (dispatch) => {
  return {
    submitConfigureUniverse: (values) => {
      dispatch(configureUniverseTemplateLoading());
      return dispatch(configureUniverseTemplate(values)).then((response) => {
        return dispatch(configureUniverseTemplateResponse(response.payload));
      });
    },

    fetchUniverseResources: (payload) => {
      dispatch(configureUniverseResources(payload)).then((resourceData) => {
        dispatch(configureUniverseResourcesResponse(resourceData.payload));
      });
    },

    submitCreateUniverse: (values) => {
      return dispatch(createUniverse(values)).then((response) => {
        dispatch(getTlsCertificates()).then((response) => {
          dispatch(getTlsCertificatesResponse(response.payload));
        });
        return dispatch(createUniverseResponse(response.payload));
      });
    },

    fetchCustomerTasks: () => {
      dispatch(fetchCustomerTasks()).then((response) => {
        if (!response.error) {
          dispatch(fetchCustomerTasksSuccess(response.payload));
        } else {
          dispatch(fetchCustomerTasksFailure(response.payload));
        }
      });
    },

    submitAddUniverseReadReplica: (values, universeUUID) => {
      dispatch(addUniverseReadReplica(values, universeUUID)).then((response) => {
        dispatch(addUniverseReadReplicaResponse(response.payload));
      });
    },

    submitEditUniverseReadReplica: (values, universeUUID) => {
      dispatch(editUniverseReadReplica(values, universeUUID)).then((response) => {
        dispatch(editUniverseReadReplicaResponse(response.payload));
      });
    },

    fetchUniverseMetadata: () => {
      dispatch(fetchUniverseMetadata());
    },

    fetchCurrentUniverse: (universeUUID) => {
      dispatch(fetchUniverseInfo(universeUUID)).then((response) => {
        dispatch(fetchUniverseInfoResponse(response.payload));
      });
    },

    closeUniverseDialog: () => {
      dispatch(closeDialog());
      dispatch(closeUniverseDialog());
    },

    submitEditUniverse: (values, universeUUID) => {
      dispatch(editUniverse(values, universeUUID)).then((response) => {
        dispatch(editUniverseResponse(response.payload));
      });
    },

    getInstanceTypeListItems: (provider) => {
      dispatch(getInstanceTypeList(provider)).then((response) => {
        dispatch(getInstanceTypeListResponse(response.payload));
      });
    },

    getKMSConfigs: () => {
      dispatch(fetchAuthConfigList()).then((response) => {
        dispatch(fetchAuthConfigListResponse(response.payload));
      });
    },

    getRegionListItems: (provider) => {
      dispatch(getRegionList(provider)).then((response) => {
        dispatch(getRegionListResponse(response.payload));
      });
    },

    setPlacementStatus: (currentStatus) => {
      dispatch(setPlacementStatus(currentStatus));
    },

    resetConfig: () => {
      dispatch(resetUniverseConfiguration());
    },

    fetchUniverseTasks: (universeUUID) => {
      dispatch(fetchUniverseTasks(universeUUID)).then((response) => {
        dispatch(fetchUniverseTasksResponse(response.payload));
      });
    },

    getExistingUniverseConfiguration: (universeDetail) => {
      dispatch(configureUniverseTemplateSuccess({ data: universeDetail }));
      dispatch(configureUniverseResources(universeDetail)).then((resourceData) => {
        dispatch(configureUniverseResourcesResponse(resourceData.payload));
      });
    },

    closeModal: () => {
      dispatch(closeDialog());
    },

    showDeleteReadReplicaModal: () => {
      dispatch(openDialog('deleteReadReplicaModal'));
    },

    showFullMoveModal: () => {
      dispatch(openDialog('fullMoveModal'));
    },

    fetchNodeInstanceList: (providerUUID) => {
      dispatch(getNodeInstancesForProvider(providerUUID)).then((response) => {
        dispatch(getNodesInstancesForProviderResponse(response.payload));
      });
    }
  };
};

const formFieldNames = [
  'formType',
  'primary.universeName',
  'primary.provider',
  'primary.providerType',
  'primary.regionList',
  'primary.numNodes',
  'primary.instanceType',
  'primary.ybSoftwareVersion',
  'primary.accessKeyCode',
  'primary.masterGFlags',
  'primary.tserverGFlags',
  'primary.instanceTags',
  'primary.diskIops',
  'primary.numVolumes',
  'primary.volumeSize',
  'primary.storageType',
  'primary.assignPublicIP',
  'primary.useTimeSync',
  'primary.enableYSQL',
  'primary.enableIPV6',
  'primary.enableYEDIS',
  'primary.enableNodeToNodeEncrypt',
  'primary.enableClientToNodeEncrypt',
  'primary.enableEncryptionAtRest',
  'primary.selectEncryptionAtRestConfig',
  'primary.tlsCertificateId',
  'primary.mountPoints',
  'primary.awsArnString',
  'async.universeName',
  'async.provider',
  'async.providerType',
  'async.regionList',
  'async.numNodes',
  'async.instanceType',
  'async.ybSoftwareVersion',
  'async.accessKeyCode',
  'async.assignPublicIP',
  'async.useTimeSync',
  'async.enableYSQL',
  'async.enableIPV6',
  'async.enableYEDIS',
  'async.enableNodeToNodeEncrypt',
  'async.enableClientToNodeEncrypt',
  'async.mountPoints',
  'masterGFlags',
  'tserverGFlags',
  'instanceTags',
  'asyncClusters'
];

function getFormData(currentUniverse, formType, clusterType) {
  const {
    universeDetails: { clusters, encryptionAtRestConfig, rootCA }
  } = currentUniverse.data;
  const cluster = getClusterByType(clusters, clusterType);
  const data = {};
  if (isDefinedNotNull(cluster)) {
    const userIntent = cluster.userIntent;
    data[clusterType] = {};
    data[clusterType].universeName = currentUniverse.data.name;
    data.formType = formType;
    data[clusterType].assignPublicIP = userIntent.assignPublicIP;
    data[clusterType].useTimeSync = userIntent.useTimeSync;
    data[clusterType].enableYSQL = userIntent.enableYSQL;
    data[clusterType].enableIPV6 = userIntent.enableIPV6;
    data[clusterType].enableYEDIS = userIntent.enableYEDIS;
    data[clusterType].enableNodeToNodeEncrypt = userIntent.enableNodeToNodeEncrypt;
    data[clusterType].enableClientToNodeEncrypt = userIntent.enableClientToNodeEncrypt;
    data[clusterType].provider = userIntent.provider;
    data[clusterType].numNodes = userIntent.numNodes;
    data[clusterType].replicationFactor = userIntent.replicationFactor;
    data[clusterType].instanceType = userIntent.instanceType;
    data[clusterType].ybSoftwareVersion = userIntent.ybSoftwareVersion;
    data[clusterType].accessKeyCode = userIntent.accessKeyCode;
    data[clusterType].diskIops = userIntent.deviceInfo.diskIops;
    data[clusterType].numVolumes = userIntent.deviceInfo.numVolumes;
    data[clusterType].volumeSize = userIntent.deviceInfo.volumeSize;
    data[clusterType].storageType = userIntent.deviceInfo.storageType;
    data[clusterType].mountPoints = userIntent.deviceInfo.mountPoints;
    data[clusterType].storageClass = userIntent.deviceInfo.storageClass;

    data[clusterType].regionList = cluster.regions.map((item) => {
      return { value: item.uuid, name: item.name, label: item.name };
    });
    data[clusterType].masterGFlags = Object.keys(userIntent.masterGFlags).map((key) => {
      return { name: key, value: userIntent.masterGFlags[key] };
    });
    data[clusterType].tserverGFlags = Object.keys(userIntent.tserverGFlags).map((key) => {
      return { name: key, value: userIntent.tserverGFlags[key] };
    });
    data[clusterType].instanceTags = Object.keys(userIntent.instanceTags).map((key) => {
      return { name: key, value: userIntent.instanceTags[key] };
    });

    data[clusterType].tlsCertificateId = rootCA;
    if (encryptionAtRestConfig) {
      data[clusterType].enableEncryptionAtRest = encryptionAtRestConfig.encryptionAtRestEnabled;
      data[clusterType].selectEncryptionAtRestConfig = encryptionAtRestConfig.kmsConfigUUID;
    }
  }
  return data;
}

function mapStateToProps(state, ownProps) {
  const {
    universe: { currentUniverse }
  } = state;
  let data = {
    formType: 'Create',
    primary: {
      universeName: '',
      ybSoftwareVersion: '',
      numNodes: 3,
      isMultiAZ: true,
      instanceType: 'c5.large',
      accessKeyCode: 'yugabyte-default',
      assignPublicIP: true,
      useTimeSync: false,
      enableYSQL: true,
      enableIPV6: false,
      enableYEDIS: false,
      enableNodeToNodeEncrypt: false,
      enableClientToNodeEncrypt: false,
      enableEncryptionAtRest: false,
      awsArnString: '',
      selectEncryptionAtRestConfig: null
    },
    async: {
      universeName: '',
      numNodes: 3,
      isMultiAZ: true,
      assignPublicIP: true,
      useTimeSync: false,
      enableYSQL: true,
      enableIPV6: false,
      enableYEDIS: false,
      enableNodeToNodeEncrypt: false,
      enableClientToNodeEncrypt: false
    }
  };

  if (isNonEmptyObject(currentUniverse.data) && ownProps.type !== 'Create') {
    // TODO (vit.pankin): don't like this type having Async in it,
    // it should be clusterType or currentView
    data = getFormData(
      currentUniverse,
      ownProps.type,
      ownProps.type === 'Async' ? 'async' : 'primary'
    );
  }

  const selector = formValueSelector('UniverseForm');

  return {
    universe: state.universe,
    modal: state.modal,
    tasks: state.tasks,
    cloud: state.cloud,
    softwareVersions: state.customer.softwareVersions,
    userCertificates: state.customer.userCertificates,
    accessKeys: state.cloud.accessKeys,
    initialValues: data,
    formValues: selector(
      state,
      'formType',
      'primary.universeName',
      'primary.provider',
      'primary.providerType',
      'primary.regionList',
      'primary.numNodes',
      'primary.instanceType',
      'primary.replicationFactor',
      'primary.ybSoftwareVersion',
      'primary.accessKeyCode',
      'primary.masterGFlags',
      'primary.tserverGFlags',
      'primary.instanceTags',
      'primary.diskIops',
      'primary.numVolumes',
      'primary.volumeSize',
      'primary.storageType',
      'primary.diskIops',
      'primary.assignPublicIP',
      'primary.mountPoints',
      'primary.useTimeSync',
      'primary.enableYSQL',
      'primary.enableIPV6',
      'primary.enableYEDIS',
      'primary.enableNodeToNodeEncrypt',
      'primary.enableClientToNodeEncrypt',
      'primary.enableEncryptionAtRest',
      'primary.selectEncryptionAtRestConfig',
      'primary.tlsCertificateId',
      'primary.awsArnString',
      'primary.masterHttpPort',
      'primary.masterRpcPort',
      'primary.tserverHttpPort',
      'primary.tserverRpcPort',
      'primary.redisHttpPort',
      'primary.redisRpcPort',
      'primary.yqlHttpPort',
      'primary.yqlRpcPort',
      'primary.ysqlHttpPort',
      'primary.ysqlRpcPort',
      'async.universeName',
      'async.provider',
      'async.providerType',
      'async.regionList',
      'async.replicationFactor',
      'async.numNodes',
      'async.instanceType',
      'async.deviceInfo',
      'async.ybSoftwareVersion',
      'async.accessKeyCode',
      'async.diskIops',
      'async.numVolumes',
      'async.volumeSize',
      'async.storageType',
      'async.assignPublicIP',
      'async.enableYSQL',
      'async.enableIPV6',
      'async.enableYEDIS',
      'async.enableNodeToNodeEncrypt',
      'async.enableClientToNodeEncrypt',
      'async.mountPoints',
      'async.useTimeSync',
      'masterGFlags',
      'tserverGFlags',
      'instanceTags'
    )
  };
}

const asyncValidate = (values, dispatch) => {
  return new Promise((resolve, reject) => {
    if (
      values.primary &&
      isNonEmptyString(values.primary.universeName) &&
      values.formType !== 'Async'
    ) {
      dispatch(checkIfUniverseExists(values.primary.universeName)).then((response) => {
        if (response.payload.status !== 200 && values.formType !== 'Edit') {
          reject({ primary: { universeName: 'Universe name already exists' } });
        } else {
          resolve();
        }
      });
    } else {
      resolve();
    }
  });
};

const validateProviderFields = (values, props, clusterType) => {
  const errors = {};
  if (isEmptyObject(values[clusterType])) {
    return errors;
  }
  const cloud = props.cloud;
  let currentProvider;
  if (isNonEmptyObject(values[clusterType]) && isNonEmptyString(values[clusterType].provider)) {
    currentProvider = cloud.providers.data.find(
      (provider) => provider.uuid === values[clusterType].provider
    );
  }

  if (clusterType === 'primary') {
    if (!isNonEmptyString(values[clusterType].universeName)) {
      errors.universeName = 'Universe Name is Required';
    }
    if (currentProvider && currentProvider.code === 'gcp') {
      const specialCharsRegex = /^[a-z0-9-]*$/;
      if (!specialCharsRegex.test(values[clusterType].universeName)) {
        errors.universeName =
          'GCP Universe name cannot contain capital letters or special characters except dashes';
      }
    }
    if (values[clusterType].enableEncryptionAtRest && !values[clusterType].selectEncryptionAtRestConfig) {
      errors.selectEncryptionAtRestConfig = 'KMS Config is Required for Encryption at Rest';
    }
  }

  if (isEmptyObject(currentProvider)) {
    errors.provider = 'Provider Value is Required';
  }
  if (!isNonEmptyArray(values[clusterType].regionList)) {
    errors.regionList = 'Region Value is Required';
  }
  if (!isDefinedNotNull(values[clusterType].instanceType)) {
    errors.instanceType = 'Instance Type is Required';
  }
  return errors;
};

const validate = (values, props) => {
  const errors = {};
  const { type } = props;
  // TODO: once we have the currentView property properly set, we should use that
  // to do appropriate validation.
  if (type === 'Create' || type === 'Edit') {
    errors.primary = validateProviderFields(values, props, 'primary');
  } else if (type === 'Async') {
    errors.async = validateProviderFields(values, props, 'async');
  }

  return errors;
};

const universeForm = reduxForm({
  form: 'UniverseForm',
  validate,
  asyncValidate,
  fields: formFieldNames
});

export default connect(mapStateToProps, mapDispatchToProps)(universeForm(UniverseForm));
