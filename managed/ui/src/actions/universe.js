// Copyright (c) YugaByte, Inc.

import axios from 'axios';
import { ROOT_URL } from '../config';
import { getCustomerEndpoint } from './common';

// Create Universe
export const CREATE_UNIVERSE = 'CREATE_NEW_UNIVERSE';
export const CREATE_UNIVERSE_RESPONSE = 'CREATE_UNIVERSE_RESPONSE';

// Edit Universe
export const EDIT_UNIVERSE = 'EDIT_UNIVERSE';
export const EDIT_UNIVERSE_RESPONSE = 'EDIT_UNIVERSE_RESPONSE';

// Get Universe
export const FETCH_UNIVERSE_INFO = 'FETCH_UNIVERSE_INFO';
export const FETCH_UNIVERSE_INFO_SUCCESS = 'FETCH_UNIVERSE_INFO_SUCCESS';
export const FETCH_UNIVERSE_INFO_FAILURE = 'FETCH_UNIVERSE_INFO_FAILURE';
export const RESET_UNIVERSE_INFO = 'RESET_UNIVERSE_INFO';
export const FETCH_UNIVERSE_INFO_RESPONSE = 'FETCH_UNIVERSE_INFO_RESPONSE';

// Get List Of Universe
export const FETCH_UNIVERSE_LIST = 'FETCH_UNIVERSE_LIST';
export const FETCH_UNIVERSE_LIST_RESPONSE = 'FETCH_UNIVERSE_LIST_RESPONSE';
export const FETCH_UNIVERSE_LIST_SUCCESS = 'FETCH_UNIVERSE_LIST_SUCCESS';
export const FETCH_UNIVERSE_LIST_FAILURE = 'FETCH_UNIVERSE_LIST_FAILURE';
export const RESET_UNIVERSE_LIST = 'RESET_UNIVERSE_LIST';

// Delete Universe
export const DELETE_UNIVERSE = 'DELETE_UNIVERSE';
export const DELETE_UNIVERSE_RESPONSE = 'DELETE_UNIVERSE_RESPONSE';

// Read replicas
export const ADD_READ_REPLICA = 'ADD_READ_REPLICA';
export const ADD_READ_REPLICA_RESPONSE = 'ADD_READ_REPLICA_RESPONSE';
export const EDIT_READ_REPLICA = 'EDIT_READ_REPLICA';
export const EDIT_READ_REPLICA_RESPONSE = 'EDIT_READ_REPLICA_RESPONSE';
export const DELETE_READ_REPLICA = 'DELETE_READ_REPLICA';
export const DELETE_READ_REPLICA_RESPONSE = 'DELETE_READ_REPLICA_RESPONSE';

// Get the Master Leader for a Universe
export const GET_MASTER_LEADER = 'GET_MASTER_LEADER';
export const GET_MASTER_LEADER_RESPONSE = 'GET_MASTER_LEADER_RESPONSE';
export const RESET_MASTER_LEADER = 'RESET_MASTER_LEADER';

// Commissioner Tasks for Universe
export const FETCH_UNIVERSE_TASKS = 'FETCH_UNIVERSE_TASKS';
export const FETCH_UNIVERSE_TASKS_RESPONSE = 'FETCH_UNIVERSE_TASKS_RESPONSE';
export const RESET_UNIVERSE_TASKS = 'RESET_UNIVERSE_TASKS';

// Universe Co-Modal Tasks
export const CLOSE_UNIVERSE_DIALOG = 'CLOSE_UNIVERSE_DIALOG';

// Submit G-Flag Tasks
export const ROLLING_UPGRADE = 'ROLLING_UPGRADE';
export const ROLLING_UPGRADE_RESPONSE = 'ROLLING_UPGRADE_RESPONSE';
export const RESET_ROLLING_UPGRADE = 'RESET_ROLLING_UPGRADE';

// Universe Template Tasks
export const CONFIGURE_UNIVERSE_TEMPLATE = 'CONFIGURE_UNIVERSE_TEMPLATE';
export const CONFIGURE_UNIVERSE_TEMPLATE_RESPONSE = 'CONFIGURE_UNIVERSE_TEMPLATE_RESPONSE';
export const CONFIGURE_UNIVERSE_TEMPLATE_SUCCESS = 'CONFIGURE_UNIVERSE_TEMPLATE_SUCCESS';
export const CONFIGURE_UNIVERSE_TEMPLATE_LOADING = 'CONFIGURE_UNIVERSE_TEMPLATE_LOADING';

export const CONFIGURE_UNIVERSE_RESOURCES = 'CONFIGURE_UNIVERSE_RESOURCES';
export const CONFIGURE_UNIVERSE_RESOURCES_RESPONSE = 'CONFIGURE_UNIVERSE_RESOURCES_RESPONSE';

// Universe per-node status
export const GET_UNIVERSE_PER_NODE_STATUS = 'GET_UNIVERSE_PER_NODE_STATUS';
export const GET_UNIVERSE_PER_NODE_STATUS_RESPONSE = 'GET_UNIVERSE_PER_NODE_STATUS_RESPONSE';
export const GET_UNIVERSE_PER_NODE_METRICS = 'GET_UNIVERSE_PER_NODE_METRICS';
export const GET_UNIVERSE_PER_NODE_METRICS_RESPONSE = 'GET_UNIVERSE_PER_NODE_METRICS_RESPONSE';

//Validation Tasks
export const CHECK_IF_UNIVERSE_EXISTS = 'CHECK_IF_UNIVERSE_EXISTS';

// Set Universe Read Write Metrics
export const SET_UNIVERSE_METRICS = 'SET_UNIVERSE_METRICS';

// Set Placement Status
export const SET_PLACEMENT_STATUS = 'SET_PLACEMENT_STATUS';

// Reset Universe Configuration
export const RESET_UNIVERSE_CONFIGURATION = 'RESET_UNIVERSE_CONFIGURATION';

export const FETCH_UNIVERSE_METADATA = 'FETCH_UNIVERSE_METADATA';

export const PERFORM_UNIVERSE_NODE_ACTION = 'PERFORM_UNIVERSE_NODE_ACTION';
export const PERFORM_UNIVERSE_NODE_ACTION_RESPONSE = 'PERFORM_UNIVERSE_NODE_ACTION_RESPONSE';

export const FETCH_UNIVERSE_BACKUPS = 'FETCH_UNIVERSE_BACKUPS';
export const FETCH_UNIVERSE_BACKUPS_RESPONSE = 'FETCH_UNIVERSE_BACKUPS_RESPONSE';
export const RESET_UNIVERSE_BACKUPS = 'RESET_UNIVERSE_BACKUPS';

export const CREATE_UNIVERSE_BACKUP = 'CREATE_UNIVERSE_BACKUP';
export const CREATE_UNIVERSE_BACKUP_RESPONSE = 'CREATE_UNIVERSE_BACKUP_RESPONSE';

// Health Checking for Universe
export const GET_HEALTH_CHECK = 'GET_HEALTH_CHECK';
export const GET_HEALTH_CHECK_RESPONSE = 'GET_HEALTH_CHECK_RESPONSE';

export const SET_ENCRYPTION_KEY = 'SET_ENCRYPTION_KEY';
export const SET_ENCRYPTION_KEY_RESPONSE = 'SET_ENCRYPTION_KEY_RESPONSE';

export const IMPORT_UNIVERSE = 'IMPORT_UNIVERSE';
export const IMPORT_UNIVERSE_INIT = 'IMPORT_UNIVERSE_INIT';
export const IMPORT_UNIVERSE_RESPONSE = 'IMPORT_UNIVERSE_RESPONSE';
export const IMPORT_UNIVERSE_RESET = 'IMPORT_UNIVERSE_RESET';

export const SET_ALERTS_CONFIG = 'SET_ALERTS_CONFIG';
export const SET_ALERTS_CONFIG_RESPONSE = 'SET_ALERTS_CONFIG_RESPONSE';

export const UPDATE_BACKUP_STATE = 'UPDATE_BACKUP_STATE';
export const UPDATE_BACKUP_STATE_RESPONSE = 'UPDATE_BACKUP_STATE_RESPONSE';

export function createUniverse(formValues) {
  const customerUUID = localStorage.getItem('customerId');
  const request = axios.post(`${ROOT_URL}/customers/${customerUUID}/universes`, formValues);
  return {
    type: CREATE_UNIVERSE,
    payload: request
  };
}

export function createUniverseResponse(response) {
  return {
    type: CREATE_UNIVERSE_RESPONSE,
    payload: response
  };
}

export function fetchUniverseInfo(universeUUID) {
  const cUUID = localStorage.getItem('customerId');
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/universes/${universeUUID}`);
  return {
    type: FETCH_UNIVERSE_INFO,
    payload: request
  };
}

export function fetchUniverseInfoSuccess(universeInfo) {
  return {
    type: FETCH_UNIVERSE_INFO_SUCCESS,
    payload: universeInfo
  };
}

export function fetchUniverseInfoFailure(error) {
  return {
    type: FETCH_UNIVERSE_INFO_FAILURE,
    payload: error
  };
}

export function resetUniverseInfo() {
  return {
    type: RESET_UNIVERSE_INFO
  };
}

export function fetchUniverseInfoResponse(response) {
  return {
    type: FETCH_UNIVERSE_INFO_RESPONSE,
    payload: response
  };
}

export function fetchUniverseList() {
  const cUUID = localStorage.getItem('customerId');
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/universes`);

  return {
    type: FETCH_UNIVERSE_LIST,
    payload: request
  };
}

export function fetchUniverseListResponse(response) {
  return {
    type: FETCH_UNIVERSE_LIST_RESPONSE,
    payload: response
  };
}

export function fetchUniverseListSuccess(universeList) {
  return {
    type: FETCH_UNIVERSE_LIST_SUCCESS,
    payload: universeList
  };
}

export function fetchUniverseListFailure(error) {
  return {
    type: FETCH_UNIVERSE_LIST_FAILURE,
    payload: error
  };
}

export function resetUniverseList() {
  return {
    type: RESET_UNIVERSE_LIST
  };
}

export function deleteUniverse(universeUUID, isForceDelete) {
  const customerUUID = localStorage.getItem('customerId');
  const deleteRequestPayload = { isForceDelete: isForceDelete };
  const request = axios.delete(`${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}`, {
    params: deleteRequestPayload
  });
  return {
    type: DELETE_UNIVERSE,
    payload: request
  };
}

export function deleteUniverseResponse(response) {
  return {
    type: DELETE_UNIVERSE_RESPONSE,
    payload: response
  };
}

export function editUniverse(formValues, universeUUID) {
  const cUUID = localStorage.getItem('customerId');
  const request = axios.put(`${ROOT_URL}/customers/${cUUID}/universes/${universeUUID}`, formValues);
  return {
    type: EDIT_UNIVERSE,
    payload: request
  };
}

export function editUniverseResponse(response) {
  return {
    type: EDIT_UNIVERSE_RESPONSE,
    payload: response
  };
}

export function addUniverseReadReplica(formValues, universeUUID) {
  const cUUID = localStorage.getItem('customerId');
  const request = axios.post(
    `${ROOT_URL}/customers/${cUUID}/universes/${universeUUID}/cluster`,
    formValues
  );
  return {
    type: ADD_READ_REPLICA,
    payload: request
  };
}

export function addUniverseReadReplicaResponse(response) {
  return {
    type: ADD_READ_REPLICA_RESPONSE,
    payload: response
  };
}

export function editUniverseReadReplica(formValues, universeUUID) {
  const cUUID = localStorage.getItem('customerId');
  const request = axios.put(`${ROOT_URL}/customers/${cUUID}/universes/${universeUUID}`, formValues);
  return {
    type: EDIT_READ_REPLICA,
    payload: request
  };
}

export function editUniverseReadReplicaResponse(response) {
  return {
    type: EDIT_READ_REPLICA_RESPONSE,
    payload: response
  };
}

export function deleteUniverseReadReplica(clusterUUID, universeUUID, isForceDelete) {
  const cUUID = localStorage.getItem('customerId');
  const deleteRequestPayload = { isForceDelete: isForceDelete };
  const request = axios.delete(
    `${ROOT_URL}/customers/${cUUID}/universes/${universeUUID}/cluster/${clusterUUID}`,
    { params: deleteRequestPayload }
  );
  return {
    type: DELETE_READ_REPLICA,
    payload: request
  };
}

export function deleteUniverseReadReplicaResponse(response) {
  return {
    type: DELETE_READ_REPLICA_RESPONSE,
    payload: response
  };
}

export function fetchUniverseTasks(universeUUID) {
  const customerUUID = localStorage.getItem('customerId');
  const request = axios.get(
    `${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}/tasks`
  );
  return {
    type: FETCH_UNIVERSE_TASKS,
    payload: request
  };
}

export function fetchUniverseTasksResponse(response) {
  return {
    type: FETCH_UNIVERSE_TASKS_RESPONSE,
    payload: response
  };
}

export function resetUniverseTasks() {
  return {
    type: RESET_UNIVERSE_TASKS
  };
}

export function closeUniverseDialog() {
  return {
    type: CLOSE_UNIVERSE_DIALOG
  };
}

export function rollingUpgrade(values, universeUUID) {
  const customerUUID = localStorage.getItem('customerId');
  const request = axios.post(
    `${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}/upgrade`,
    values
  );
  return {
    type: ROLLING_UPGRADE,
    payload: request
  };
}

export function rollingUpgradeResponse(response) {
  return {
    type: ROLLING_UPGRADE_RESPONSE,
    payload: response
  };
}

export function configureUniverseTemplateLoading() {
  return {
    type: CONFIGURE_UNIVERSE_TEMPLATE_LOADING
  };
}

export function configureUniverseTemplate(values) {
  const customerUUID = localStorage.getItem('customerId');
  const request = axios.post(`${ROOT_URL}/customers/${customerUUID}/universe_configure`, values);
  return {
    type: CONFIGURE_UNIVERSE_TEMPLATE,
    payload: request
  };
}

export function configureUniverseTemplateResponse(response) {
  return {
    type: CONFIGURE_UNIVERSE_TEMPLATE_RESPONSE,
    payload: response
  };
}

export function configureUniverseTemplateSuccess(result) {
  return {
    type: CONFIGURE_UNIVERSE_TEMPLATE_SUCCESS,
    payload: result
  };
}

export function configureUniverseResources(values) {
  const customerUUID = localStorage.getItem('customerId');
  const request = axios.post(`${ROOT_URL}/customers/${customerUUID}/universe_resources`, values);
  return {
    type: CONFIGURE_UNIVERSE_RESOURCES,
    payload: request
  };
}

export function configureUniverseResourcesResponse(response) {
  return {
    type: CONFIGURE_UNIVERSE_RESOURCES_RESPONSE,
    payload: response
  };
}

export function getUniversePerNodeStatus(universeUUID) {
  const requestUrl = `${getCustomerEndpoint()}/universes/${universeUUID}/status`;
  const request = axios.get(requestUrl);
  return {
    type: GET_UNIVERSE_PER_NODE_STATUS,
    payload: request
  };
}

export function getUniversePerNodeStatusResponse(response) {
  return {
    type: GET_UNIVERSE_PER_NODE_STATUS_RESPONSE,
    payload: response
  };
}

export function getUniversePerNodeMetrics(universeUUID) {
  const requestUrl = `${getCustomerEndpoint()}/universes/${universeUUID}/tablet-servers`;
  const request = axios.get(requestUrl);
  return {
    type: GET_UNIVERSE_PER_NODE_METRICS,
    payload: request
  };
}

export function getUniversePerNodeMetricsResponse(response) {
  return {
    type: GET_UNIVERSE_PER_NODE_METRICS_RESPONSE,
    payload: response
  };
}

export function performUniverseNodeAction(universeUUID, nodeName, actionType) {
  const requestUrl = `${getCustomerEndpoint()}/universes/${universeUUID}/nodes/${nodeName}`;
  const request = axios.put(requestUrl, { nodeAction: actionType });
  return {
    type: PERFORM_UNIVERSE_NODE_ACTION,
    payload: request
  };
}

export function performUniverseNodeActionResponse(response) {
  return {
    type: PERFORM_UNIVERSE_NODE_ACTION_RESPONSE,
    payload: response
  };
}

export function getMasterLeader(universeUUID) {
  const request = axios.get(`${getCustomerEndpoint()}/universes/${universeUUID}/leader`);
  return {
    type: GET_MASTER_LEADER,
    payload: request
  };
}

export function getMasterLeaderResponse(response) {
  return {
    type: GET_MASTER_LEADER_RESPONSE,
    payload: response
  };
}

export function resetMasterLeader() {
  return {
    type: RESET_MASTER_LEADER
  };
}

export function checkIfUniverseExists(universeName) {
  const customerUUID = localStorage.getItem('customerId');
  const requestUrl = `${ROOT_URL}/customers/${customerUUID}/universes/find/${universeName}`;
  const request = axios.get(requestUrl);
  return {
    type: CHECK_IF_UNIVERSE_EXISTS,
    payload: request
  };
}

export function resetRollingUpgrade() {
  return {
    type: RESET_ROLLING_UPGRADE
  };
}

export function setUniverseMetrics(values) {
  return {
    type: SET_UNIVERSE_METRICS,
    payload: values
  };
}

export function setPlacementStatus(currentStatus) {
  return {
    type: SET_PLACEMENT_STATUS,
    payload: currentStatus
  };
}

export function resetUniverseConfiguration() {
  return {
    type: RESET_UNIVERSE_CONFIGURATION
  };
}

export function fetchUniverseMetadata() {
  return {
    type: FETCH_UNIVERSE_METADATA
  };
}

export function fetchUniverseBackups(universeUUID) {
  const cUUID = localStorage.getItem('customerId');
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/universes/${universeUUID}/backups`);
  return {
    type: FETCH_UNIVERSE_BACKUPS,
    payload: request
  };
}

export function fetchUniverseBackupsResponse(response) {
  return {
    type: FETCH_UNIVERSE_BACKUPS_RESPONSE,
    payload: response
  };
}

export function resetUniverseBackups() {
  return {
    type: RESET_UNIVERSE_BACKUPS
  };
}

export function createUniverseBackup(universeUUID, formValues) {
  const cUUID = localStorage.getItem('customerId');
  const request = axios.put(
    `${ROOT_URL}/customers/${cUUID}/universes/${universeUUID}/multi_table_backup`,
    formValues
  );
  return {
    type: CREATE_UNIVERSE_BACKUP,
    payload: request
  };
}

export function createUniverseBackupResponse(response) {
  return {
    type: CREATE_UNIVERSE_BACKUP_RESPONSE,
    payload: response
  };
}

export function getHealthCheck(universeUUID) {
  const customerUUID = localStorage.getItem('customerId');
  const request = axios.get(
    `${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}/health_check`
  );
  return {
    type: GET_HEALTH_CHECK,
    payload: request
  };
}

export function getHealthCheckResponse(response) {
  return {
    type: GET_HEALTH_CHECK_RESPONSE,
    payload: response
  };
}

export function setEncryptionKey(universeUUID, data) {
  const customerUUID = localStorage.getItem('customerId');
  const endpoint = `${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}/set_key`;
  const request = axios.post(endpoint, data);
  return {
    type: SET_ENCRYPTION_KEY,
    payload: request
  };
}

export function setEncryptionKeyResponse(response) {
  return {
    type: SET_ENCRYPTION_KEY_RESPONSE,
    payload: response
  };
}

export function importUniverseInit() {
  return {
    type: IMPORT_UNIVERSE_INIT
  };
}

export function importUniverse(values) {
  const customerUUID = localStorage.getItem('customerId');
  const request = axios.post(`${ROOT_URL}/customers/${customerUUID}/universes/import`, values);
  return {
    type: IMPORT_UNIVERSE,
    payload: request
  };
}

export function importUniverseResponse(response) {
  return {
    type: IMPORT_UNIVERSE_RESPONSE,
    payload: response
  };
}

export function importUniverseReset() {
  return {
    type: IMPORT_UNIVERSE_RESET
  };
}

export function setAlertsConfig(universeUUID, data) {
  const customerUUID = localStorage.getItem('customerId');
  const endpoint = `${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}/config_alerts`;
  const request = axios.post(endpoint, data);
  return {
    type: SET_ALERTS_CONFIG,
    payload: request
  };
}

export function setAlertsConfigResponse(response) {
  return {
    type: SET_ALERTS_CONFIG_RESPONSE,
    payload: response
  };
}

export function updateBackupState(universeUUID, flag) {
  const customerUUID = localStorage.getItem('customerId');
  const endpoint = `${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}/update_backup_state?markActive=${flag}`;
  const request = axios.put(endpoint);

  return {
    type: UPDATE_BACKUP_STATE,
    payload: request
  };
}

export function updateBackupStateResponse(response) {
  return {
    type: UPDATE_BACKUP_STATE_RESPONSE,
    payload: response
  };
}

export function fetchLiveQueries(universeUUID, cancelFn) {
  const customerUUID = localStorage.getItem("customerId");
  const endpoint = `${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}/live_queries`;
  let request;
  if (cancelFn) {
    const CancelToken = axios.CancelToken;
    request = axios.get(endpoint, {
      cancelToken: new CancelToken(cancelFn)
    });
  } else {
    request = axios.get(endpoint);
  }

  return request;
}

export function createAlertDefinition(universeUUID, data) {
  const customerUUID = localStorage.getItem('customerId');
  const endpoint = `${ROOT_URL}/customers/${customerUUID}/universes/${universeUUID}/alert_definitions`;
  return axios.post(endpoint, data);
}

export function getAlertDefinition(universeUUID, alertName) {
  const customerUUID = localStorage.getItem('customerId');
  const endpoint = `${ROOT_URL}/customers/${customerUUID}/alert_definitions/${universeUUID}/${alertName}`;
  return axios.get(endpoint).then(resp => resp.data);
}

export function updateAlertDefinition(alertDefinitionUUID, data) {
  const customerUUID = localStorage.getItem('customerId');
  const endpoint = `${ROOT_URL}/customers/${customerUUID}/alert_definitions/${alertDefinitionUUID}`;
  return axios.put(endpoint, data);
}
