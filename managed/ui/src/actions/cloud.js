// Copyright (c) YugaByte, Inc.

import axios from 'axios';
import { ROOT_URL, PROVIDER_TYPES } from '../config';

import { getProviderEndpoint, getCustomerEndpoint } from './common';

// Get Region List
export const GET_REGION_LIST = 'GET_REGION_LIST';
export const GET_REGION_LIST_RESPONSE = 'GET_REGION_LIST_RESPONSE';

// Get Provider List
export const GET_PROVIDER_LIST = 'GET_PROVIDER_LIST';
export const GET_PROVIDER_LIST_RESPONSE = 'GET_PROVIDER_LIST_RESPONSE';

// Get Instance Type List
export const GET_INSTANCE_TYPE_LIST = 'GET_INSTANCE_TYPE_LIST';
export const GET_INSTANCE_TYPE_LIST_RESPONSE = 'GET_INSTANCE_TYPE_LIST_RESPONSE';

export const RESET_PROVIDER_LIST = 'RESET_PROVIDER_LIST';

export const GET_SUPPORTED_REGION_DATA = 'GET_SUPPORTED_REGION_DATA';
export const GET_SUPPORTED_REGION_DATA_RESPONSE = 'GET_SUPPORTED_REGION_DATA_RESPONSE';

export const CREATE_PROVIDER = 'CREATE_PROVIDER';
export const CREATE_PROVIDER_RESPONSE = 'CREATE_PROVIDER_RESPONSE';

// UI bootstrap for On-Prem provider, will be removed when OnPrem moves to Yugaware side Bootstrap
export const CREATE_ONPREM_PROVIDER = 'CREATE_ONPREM_PROVIDER';
export const CREATE_ONPREM_PROVIDER_RESPONSE = 'CREATE_ONPREM_PROVIDER_RESPONSE';

export const CREATE_INSTANCE_TYPE = 'CREATE_INSTANCE_TYPE';
export const CREATE_INSTANCE_TYPE_RESPONSE = 'CREATE_INSTANCE_TYPE_RESPONSE';

export const CREATE_REGION = 'CREATE_REGION';
export const CREATE_REGION_RESPONSE = 'CREATE_REGION_RESPONSE';

export const CREATE_ZONES = 'CREATE_ZONES';
export const CREATE_ZONES_RESPONSE = 'CREATE_ZONES_RESPONSE';

export const CREATE_NODE_INSTANCES = 'CREATE_NODE_INSTANCES';
export const CREATE_NODE_INSTANCES_RESPONSE = 'CREATE_NODE_INSTANCE_RESPONSES';

export const CREATE_ACCESS_KEY = 'CREATE_ACCESS_KEY';
export const CREATE_ACCESS_KEY_RESPONSE = 'CREATE_ACCESS_KEY_RESPONSE';
export const CREATE_ACCESS_KEY_FAILURE = 'CREATE_ACCESS_KEY_FAILURE';

export const INITIALIZE_PROVIDER = 'INITIALIZE_PROVIDER';
export const INITIALIZE_PROVIDER_SUCCESS = 'INITIALIZE_PROVIDER_SUCCESS';
export const INITIALIZE_PROVIDER_FAILURE = 'INITIALIZE_PROVIDER_FAILURE';

export const DELETE_PROVIDER = 'DELETE_PROVIDER';
export const DELETE_PROVIDER_SUCCESS = 'DELETE_PROVIDER_SUCCESS';
export const DELETE_PROVIDER_FAILURE = 'DELETE_PROVIDER_FAILURE';
export const DELETE_PROVIDER_RESPONSE = 'DELETE_PROVIDER_RESPONSE';

export const RESET_PROVIDER_BOOTSTRAP = 'RESET_PROVIDER_BOOTSTRAP';

export const LIST_ACCESS_KEYS = 'LIST_ACCESS_KEYS';
export const LIST_ACCESS_KEYS_RESPONSE = 'LIST_ACCESS_KEYS_RESPONSE';

export const GET_EBS_TYPE_LIST = 'GET_EBS_TYPES';
export const GET_EBS_TYPE_LIST_RESPONSE = 'GET_EBS_TYPES_RESPONSE';

export const GET_GCP_TYPE_LIST = 'GET_GCP_TYPES';
export const GET_GCP_TYPE_LIST_RESPONSE = 'GET_GCP_TYPES_RESPONSE';

export const GET_AZU_TYPE_LIST = 'GET_AZU_TYPES';
export const GET_AZU_TYPE_LIST_RESPONSE = 'GET_AZU_TYPES_RESPONSE';

export const CREATE_DOCKER_PROVIDER = 'CREATE_DOCKER_PROVIDER';
export const CREATE_DOCKER_PROVIDER_RESPONSE = 'CREATE_DOCKER_PROVIDER_RESPONSE';

export const FETCH_CLOUD_METADATA = 'FETCH_CLOUD_METADATA';

export const SET_ON_PREM_CONFIG_DATA = 'SET_ON_PREM_CONFIG_DATA';

export const GET_NODE_INSTANCE_LIST = 'GET_NODE_INSTANCE';
export const GET_NODE_INSTANCE_LIST_RESPONSE = 'GET_NODE_INSTANCE_RESPONSE';

export const RESET_ON_PREM_CONFIG_DATA = 'RESET_ON_PREM_CONFIG_DATA';

export const BOOTSTRAP_PROVIDER = 'BOOTSTRAP_PROVIDER';
export const BOOTSTRAP_PROVIDER_RESPONSE = 'BOOTSTRAP_PROVIDER_RESPONSE';

export const DELETE_INSTANCE = 'DELETE_INSTANCE';
export const DELETE_INSTANCE_RESPONSE = 'DELETE_INSTANCE_RESPONSE';

export const EDIT_PROVIDER = 'EDIT_PROVIDER';
export const EDIT_PROVIDER_RESPONSE = 'EDIT_PROVIDER_RESPONSE';

export const FETCH_AUTH_CONFIG = 'FETCH_AUTH_CONFIG';
export const FETCH_AUTH_CONFIG_RESPONSE = 'FETCH_AUTH_CONFIG_RESPONSE';

export const CREATE_KMS_CONFIGURATION = 'CREATE_KMS_CONFIGURATION';
export const CREATE_KMS_CONFIGURATION_RESPONSE = 'CREATE_KMS_CONFIGURATION_RESPONSE';

export const DELETE_KMS_CONFIGURATION = 'DELETE_KMS_CONFIGURATION';
export const DELETE_KMS_CONFIGURATION_RESPONSE = 'DELETE_KMS_CONFIGURATION_RESPONSE';

export function getProviderList() {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/providers`);
  return {
    type: GET_PROVIDER_LIST,
    payload: request
  };
}

export function getProviderListResponse(responsePayload) {
  return {
    type: GET_PROVIDER_LIST_RESPONSE,
    payload: responsePayload
  };
}

export function getRegionList(providerUUID) {
  const baseUrl = getProviderEndpoint(providerUUID);
  const request = axios.get(`${baseUrl}/regions`);
  return {
    type: GET_REGION_LIST,
    payload: request
  };
}

export function getRegionListResponse(responsePayload) {
  return {
    type: GET_REGION_LIST_RESPONSE,
    payload: responsePayload
  };
}

export function getInstanceTypeList(providerUUID) {
  const url = getProviderEndpoint(providerUUID) + '/instance_types';
  const request = axios.get(url);
  return {
    type: GET_INSTANCE_TYPE_LIST,
    payload: request
  };
}

export function getInstanceTypeListResponse(responsePayload) {
  return {
    type: GET_INSTANCE_TYPE_LIST_RESPONSE,
    payload: responsePayload
  };
}

export function createInstanceType(providerCode, providerUUID, instanceTypeInfo) {
  const formValues = {
    'idKey': {
      'providerCode': providerCode,
      'instanceTypeCode': instanceTypeInfo.instanceTypeCode
    },
    'numCores': instanceTypeInfo.numCores,
    'memSizeGB': instanceTypeInfo.memSizeGB,
    'instanceTypeDetails': {
      'volumeDetailsList': instanceTypeInfo.volumeDetailsList
    }
  };
  const url = getProviderEndpoint(providerUUID) + '/instance_types';
  const request = axios.post(url, formValues);
  return {
    type: CREATE_INSTANCE_TYPE,
    payload: request
  };
}

export function createInstanceTypeResponse(responsePayload) {
  return {
    type: CREATE_INSTANCE_TYPE_RESPONSE,
    payload: responsePayload
  };
}

export function getSupportedRegionData() {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/regions`);
  return {
    type: GET_SUPPORTED_REGION_DATA,
    payload: request
  };
}

export function getSupportedRegionDataResponse(responsePayload) {
  return {
    type: GET_SUPPORTED_REGION_DATA_RESPONSE,
    payload: responsePayload
  };
}

export function resetProviderList() {
  return {
    type: RESET_PROVIDER_LIST
  };
}

export function createProvider(type, name, config) {
  const customerUUID = localStorage.getItem("customerId");
  const provider = PROVIDER_TYPES.find( (providerType) => providerType.code === type );
  const formValues = {
    'code': provider.code,
    'name': name,
    'config': config
  };
  const request = axios.post(`${ROOT_URL}/customers/${customerUUID}/providers`, formValues);
  return {
    type: CREATE_PROVIDER,
    payload: request
  };
}

/**
 *  New create provider API for multi-region and multi-zone configurations.
 *  Currently, only supports Kubernetes.
 */
export function createMultiRegionKubernetesProvider(name, config, regions) {
  const customerUUID = localStorage.getItem("customerId");
  const formValues = {
    code: 'kubernetes',
    name: name,
    config: config,
    regionList: regions,
  };
  const request = axios.post(`${ROOT_URL}/customers/${customerUUID}/providers/kubernetes`, formValues);
  return {
    type: CREATE_PROVIDER,
    payload: request
  };
}

export function createProviderResponse(result) {
  return {
    type: CREATE_PROVIDER_RESPONSE,
    payload: result
  };
}

export function createRegion(providerUUID, formValues) {
  const url = getProviderEndpoint(providerUUID) + '/regions';
  const request = axios.post(url, formValues);
  return {
    type: CREATE_REGION,
    payload: request
  };
}

export function createRegionResponse(result) {
  return {
    type: CREATE_REGION_RESPONSE,
    payload: result
  };
}

export function createZones(providerUUID, regionUUID, zones) {
  const formValues = {
    "availabilityZones": zones.map((zone) => {
      return {"code": zone, "name": zone };
    })
  };
  const url = getProviderEndpoint(providerUUID) + '/regions/' + regionUUID + '/zones';
  const request = axios.post(url, formValues);
  return {
    type: CREATE_ZONES,
    payload: request
  };
}

export function createZonesResponse(result) {
  return {
    type: CREATE_ZONES_RESPONSE,
    payload: result
  };
}

export function createNodeInstances(zoneUUID, nodes) {
  const customerUUID = localStorage.getItem("customerId");
  const url = `${ROOT_URL}/customers/${customerUUID}/zones/${zoneUUID}/nodes`;
  const formValues = { "nodes": nodes };
  const request = axios.post(url, formValues);
  return {
    type: CREATE_NODE_INSTANCES,
    payload: request
  };
}

export function createNodeInstancesResponse(result) {
  return {
    type: CREATE_NODE_INSTANCES_RESPONSE,
    payload: result
  };
}

export function createAccessKey(providerUUID, regionUUID, keyInfo) {
  const formValues = {
    keyCode: keyInfo.code,
    regionUUID: regionUUID,
    keyType: "PRIVATE",
    keyContent: keyInfo.privateKeyContent,
    sshUser: keyInfo.sshUser,
    sshPort: keyInfo.sshPort,
    passwordlessSudoAccess: keyInfo.passwordlessSudoAccess,
    airGapInstall: keyInfo.airGapInstall
  };
  const url = getProviderEndpoint(providerUUID) + '/access_keys';
  const request = axios.post(url, formValues);
  return {
    type: CREATE_ACCESS_KEY,
    payload: request
  };
}

export function createAccessKeyResponse(result) {
  return {
    type: CREATE_ACCESS_KEY_RESPONSE,
    payload: result
  };
}

export function createAccessKeyFailure(error) {
  return {
    type: CREATE_ACCESS_KEY_FAILURE,
    payload: error
  };
}

export function fetchAuthConfigList() {
  const endpoint = getCustomerEndpoint() + '/kms_configs';
  const request = axios.get(endpoint);
  return {
    type: FETCH_AUTH_CONFIG,
    payload: request
  };
}

export function fetchAuthConfigListResponse(result) {
  return {
    type: FETCH_AUTH_CONFIG_RESPONSE,
    payload: result
  };
}

export function createKMSProviderConfig(provider, body) {
  const endpoint = getCustomerEndpoint() + `/kms_configs/${provider}`;
  const request = axios.post(endpoint, body);
  return {
    type: CREATE_KMS_CONFIGURATION,
    payload: request
  };
}

export function createKMSProviderConfigResponse(result) {
  return {
    type: CREATE_KMS_CONFIGURATION_RESPONSE,
    payload: result
  };
}

export function deleteKMSProviderConfig(configUUID) {
  const endpoint = getCustomerEndpoint() + `/kms_configs/${configUUID}`;
  const request = axios.delete(endpoint);
  return {
    type: DELETE_KMS_CONFIGURATION,
    payload: request
  };
}

export function deleteKMSProviderConfigResponse(provider) {
  return {
    type: DELETE_KMS_CONFIGURATION_RESPONSE,
    payload: provider,
  };
}

export function initializeProvider(providerUUID) {
  const url = getProviderEndpoint(providerUUID) + '/initialize';
  const request = axios.get(url);
  return {
    type: INITIALIZE_PROVIDER,
    payload: request
  };
}

export function initializeProviderSuccess(result) {
  return {
    type: INITIALIZE_PROVIDER_SUCCESS,
    payload: result
  };
}

export function initializeProviderFailure(error) {
  return {
    type: INITIALIZE_PROVIDER_FAILURE,
    payload: error
  };
}

export function deleteProvider(providerUUID) {
  const cUUID = localStorage.getItem("customerId");
  const request =
    axios.delete(`${ROOT_URL}/customers/${cUUID}/providers/${providerUUID}`);
  return {
    type: DELETE_PROVIDER,
    payload: request
  };
}

export function deleteProviderSuccess(data) {
  return {
    type: DELETE_PROVIDER_SUCCESS,
    payload: data
  };
}

export function deleteProviderFailure(error) {
  return {
    type: DELETE_PROVIDER_FAILURE,
    payload: error
  };
}

export function deleteProviderResponse(response) {
  return {
    type: DELETE_PROVIDER_RESPONSE,
    payload: response
  };
}

export function resetProviderBootstrap() {
  return {
    type: RESET_PROVIDER_BOOTSTRAP
  };
}

export function listAccessKeys(providerUUID) {
  const url = getProviderEndpoint(providerUUID) + '/access_keys';
  const request = axios.get(url);
  return {
    type: LIST_ACCESS_KEYS,
    payload: request
  };
}

export function listAccessKeysResponse(response) {
  return {
    type: LIST_ACCESS_KEYS_RESPONSE,
    payload: response
  };
}

export function getEBSTypeList() {
  const request = axios.get(`${ROOT_URL}/metadata/ebs_types`);
  return {
    type: GET_EBS_TYPE_LIST,
    payload: request
  };
}

export function getEBSTypeListResponse(responsePayload) {
  return {
    type: GET_EBS_TYPE_LIST_RESPONSE,
    payload: responsePayload
  };
}

export function getGCPTypeList() {
  const request = axios.get(`${ROOT_URL}/metadata/gcp_types`);
  return {
    type: GET_GCP_TYPE_LIST,
    payload: request
  };
}

export function getGCPTypeListResponse(responsePayload) {
  return {
    type: GET_GCP_TYPE_LIST_RESPONSE,
    payload: responsePayload
  };
}

export function getAZUTypeList() {
  const request = axios.get(`${ROOT_URL}/metadata/azu_types`);
  return {
    type: GET_AZU_TYPE_LIST,
    payload: request
  };
}

export function getAZUTypeListResponse(responsePayload) {
  return {
    type: GET_AZU_TYPE_LIST_RESPONSE,
    payload: responsePayload
  };
}

export function createDockerProvider() {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.post(`${ROOT_URL}/customers/${cUUID}/providers/setup_docker`);
  return {
    type: CREATE_DOCKER_PROVIDER,
    payload: request
  };
}

export function createDockerProviderResponse(response) {
  return {
    type: CREATE_DOCKER_PROVIDER_RESPONSE,
    payload: response
  };
}

export function fetchCloudMetadata() {
  return {
    type: FETCH_CLOUD_METADATA
  };
}

export function setOnPremConfigData(configData) {
  return {
    type: SET_ON_PREM_CONFIG_DATA,
    payload: configData
  };
}

export function getNodeInstancesForProvider(pUUID) {
  const cUUID = localStorage.getItem("customerId");
  const request = axios.get(`${ROOT_URL}/customers/${cUUID}/providers/${pUUID}/nodes/list`);
  return {
    type: GET_NODE_INSTANCE_LIST,
    payload: request
  };
}

export function getNodesInstancesForProviderResponse(response) {
  return {
    type: GET_NODE_INSTANCE_LIST_RESPONSE,
    payload: response
  };
}

export function resetOnPremConfigData() {
  return {
    type: RESET_ON_PREM_CONFIG_DATA
  };
}

export function bootstrapProvider(pUUID, params) {
  const request = axios.post(`${getProviderEndpoint(pUUID)}/bootstrap`, params);
  return {
    type: BOOTSTRAP_PROVIDER,
    payload: request
  };
}

export function bootstrapProviderResponse(response) {
  return {
    type: BOOTSTRAP_PROVIDER_RESPONSE,
    payload: response
  };
}

export function createOnPremProvider(type, name, config) {
  const formValues = {
    'code': type,
    'name': name,
    'config': config
  };
  const request = axios.post(`${getCustomerEndpoint()}/providers`, formValues);
  return {
    type: CREATE_ONPREM_PROVIDER,
    payload: request
  };
}

export function createOnPremProviderResponse(response) {
  return {
    type: CREATE_ONPREM_PROVIDER_RESPONSE,
    payload: response
  };
}

export function deleteInstance(providerUUID, instanceIP) {
  const uri = `${getProviderEndpoint(providerUUID)}/instances/${instanceIP}`;
  const request = axios.delete(uri);
  return {
    type: DELETE_INSTANCE,
    payload: request
  };
}

export function deleteInstanceResponse(response) {
  return {
    type: DELETE_INSTANCE_RESPONSE,
    payload: response
  };
}

export function editProvider(payload) {
  const cUUID = localStorage.getItem("customerId");
  const pUUID = payload.accountUUID;
  const request = axios.put(`${ROOT_URL}/customers/${cUUID}/providers/${pUUID}/edit`, payload);
  return {
    type: EDIT_PROVIDER,
    payload: request
  };
}

export function editProviderResponse(response) {
  return {
    type: EDIT_PROVIDER_RESPONSE,
    payload: response
  };
}
