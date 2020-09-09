// Copyright (c) YugaByte, Inc.

import { GET_REGION_LIST, GET_REGION_LIST_RESPONSE, GET_PROVIDER_LIST, GET_PROVIDER_LIST_RESPONSE,
  GET_INSTANCE_TYPE_LIST, GET_INSTANCE_TYPE_LIST_RESPONSE, RESET_PROVIDER_LIST,
  GET_SUPPORTED_REGION_DATA, GET_SUPPORTED_REGION_DATA_RESPONSE, CREATE_PROVIDER,
  CREATE_PROVIDER_RESPONSE, CREATE_REGION, CREATE_REGION_RESPONSE, CREATE_ACCESS_KEY,
  CREATE_ACCESS_KEY_RESPONSE, CREATE_ACCESS_KEY_FAILURE, INITIALIZE_PROVIDER, INITIALIZE_PROVIDER_SUCCESS,
  INITIALIZE_PROVIDER_FAILURE, DELETE_PROVIDER, DELETE_PROVIDER_SUCCESS, DELETE_PROVIDER_FAILURE,
  DELETE_PROVIDER_RESPONSE, RESET_PROVIDER_BOOTSTRAP, LIST_ACCESS_KEYS, LIST_ACCESS_KEYS_RESPONSE,
  GET_EBS_TYPE_LIST, GET_EBS_TYPE_LIST_RESPONSE, GET_GCP_TYPE_LIST, GET_GCP_TYPE_LIST_RESPONSE, CREATE_DOCKER_PROVIDER,
  CREATE_DOCKER_PROVIDER_RESPONSE,CREATE_INSTANCE_TYPE, CREATE_INSTANCE_TYPE_RESPONSE,
  FETCH_CLOUD_METADATA, CREATE_ZONES, CREATE_ZONES_RESPONSE, CREATE_NODE_INSTANCES,
  CREATE_NODE_INSTANCES_RESPONSE, SET_ON_PREM_CONFIG_DATA, GET_NODE_INSTANCE_LIST,
  GET_NODE_INSTANCE_LIST_RESPONSE, RESET_ON_PREM_CONFIG_DATA, BOOTSTRAP_PROVIDER, BOOTSTRAP_PROVIDER_RESPONSE,
  CREATE_ONPREM_PROVIDER, CREATE_ONPREM_PROVIDER_RESPONSE, EDIT_PROVIDER, EDIT_PROVIDER_RESPONSE,
  FETCH_AUTH_CONFIG, FETCH_AUTH_CONFIG_RESPONSE, DELETE_KMS_CONFIGURATION, DELETE_KMS_CONFIGURATION_RESPONSE,
  GET_AZU_TYPE_LIST, GET_AZU_TYPE_LIST_RESPONSE
} from '../actions/cloud';

import { getInitialState, setInitialState, setSuccessState, setFailureState, setLoadingState, setPromiseResponse }
  from '../utils/PromiseUtils';
import {isNonEmptyArray, isDefinedNotNull, sortInstanceTypeList} from '../utils/ObjectUtils';
import _ from 'lodash';

const INITIAL_STATE = {
  regions: getInitialState([]),
  providers: getInitialState([]),
  instanceTypes: getInitialState([]),
  supportedRegionList: getInitialState([]),
  authConfig: getInitialState([]),
  onPremJsonFormData: {},
  ebsTypes: [],
  gcpTypes: [],
  azuTypes: [],
  loading: {
    regions: false,
    providers: false,
    instanceTypes: false,
    ebsTypes: true,
    gcpTypes: true,
    azuTypes: true,
    supportedRegions: false
  },
  selectedProvider: null,
  error: null,
  accessKeys: getInitialState([]),
  bootstrap: getInitialState({}),
  dockerBootstrap: getInitialState({}),
  status : 'init',
  fetchMetadata: false,
  nodeInstanceList: getInitialState([]),
  createProvider: getInitialState({}),
  bootstrapProvider: getInitialState({})
};

export default function(state = INITIAL_STATE, action) {
  let error;
  switch(action.type) {

    case GET_PROVIDER_LIST:
      return {...setLoadingState(state, "providers", []), fetchMetadata: false};
    case GET_PROVIDER_LIST_RESPONSE:
      if (action.payload.status !== 200) {
        if (isDefinedNotNull(action.payload.data)) {
          return {...setFailureState(state, "providers", action.payload.response.data.error), fetchMetadata: false};
        } else {
          return state;
        }
      }
      return {...setSuccessState(state, "providers", action.payload.data), fetchMetadata: false};

    case GET_REGION_LIST:
      return setLoadingState(state, "regions", []);
    case GET_REGION_LIST_RESPONSE:
      if (action.payload.status !== 200) {
        return setFailureState(state, "regions", action.payload.response.data.error);
      }
      return setSuccessState(state, "regions", _.sortBy(action.payload.data, "name"));

    case GET_INSTANCE_TYPE_LIST:
      return setLoadingState(state, "instanceTypes", []);
    case GET_INSTANCE_TYPE_LIST_RESPONSE:
      if (action.payload.status !== 200) {
        return setFailureState(state, "instanceTypes", action.payload.response.data.error);
      }
      return setSuccessState(state, "instanceTypes", sortInstanceTypeList(action.payload.data));

    case RESET_PROVIDER_LIST:
      return { ...state, providers: getInitialState([]), regions: getInitialState([]), instanceTypes:getInitialState([])};

    case GET_SUPPORTED_REGION_DATA:
      return setLoadingState(state, "supportedRegionList", []);
    case GET_SUPPORTED_REGION_DATA_RESPONSE:
      if (action.payload.status !== 200) {
        return setFailureState(state, "supportedRegionList", action.payload.data ? action.payload.data.error : "Unable to fetch resources");
      }
      return setSuccessState(state, "supportedRegionList", _.sortBy(action.payload.data, "name"));

    case CREATE_PROVIDER:
      return setLoadingState(state, "createProvider", {});
    case CREATE_PROVIDER_RESPONSE:
      return setPromiseResponse(state, "createProvider", action);

    case CREATE_ONPREM_PROVIDER:
      return setLoadingState(state, "bootstrap", {type: "provider", response: null});
    case CREATE_ONPREM_PROVIDER_RESPONSE:
      if (action.payload.status === 200) {
        return setSuccessState(state, "bootstrap", {type: "provider", response: action.payload.data});
      }
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "provider"});
    case BOOTSTRAP_PROVIDER:
      return setLoadingState(state, "bootstrapProvider", {});
    case BOOTSTRAP_PROVIDER_RESPONSE:
      return setPromiseResponse(state, "bootstrapProvider", action);

    case CREATE_INSTANCE_TYPE:
      return setLoadingState(state, "bootstrap", {type: "instanceType", response: null});
    case CREATE_INSTANCE_TYPE_RESPONSE:
      if (action.payload.status === 200) {
        return setSuccessState(state, "bootstrap", {type: "instanceType", response: action.payload.data});
      }
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "instanceType"});

    case CREATE_REGION:
      return setLoadingState(state, "bootstrap", {type: "region", response: null});
    case CREATE_REGION_RESPONSE:
      if (action.payload.status === 200) {
        return setSuccessState(state, "bootstrap", {type: "region", response: action.payload.data});
      }
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "region"});

    case CREATE_ZONES:
      return setLoadingState(state, "bootstrap", {type: "zones", response: null});
    case CREATE_ZONES_RESPONSE:
      if (action.payload.status === 200) {
        return setSuccessState(state, "bootstrap", {type: "zones", response: action.payload.data});
      }
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "zone"});

    case CREATE_NODE_INSTANCES:
      return setLoadingState(state, "bootstrap", {type: "node", response: null});
    case CREATE_NODE_INSTANCES_RESPONSE:
      if (action.payload.status === 200) {
        return setSuccessState(state, "bootstrap", {type: "node", response: action.payload.data});
      }
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "node"});

    case CREATE_ACCESS_KEY:
      return setLoadingState(state, "bootstrap", {type: "accessKey", response: null});
    case CREATE_ACCESS_KEY_RESPONSE:
      if (action.payload.status === 200) {
        return setSuccessState(state, "bootstrap", {type: "accessKey", response: action.payload.data});
      }
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "accessKey"});
    case CREATE_ACCESS_KEY_FAILURE:
      return setFailureState(state, "bootstrap", action.payload, {type: "accessKey"});

    case INITIALIZE_PROVIDER:
      return setLoadingState(state, "bootstrap", {type: "initialize", response: null});
    case INITIALIZE_PROVIDER_SUCCESS:
      return setSuccessState(state, "bootstrap", {type: "initialize", response: action.payload.data});
    case INITIALIZE_PROVIDER_FAILURE:
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "initialize"});

    case DELETE_PROVIDER:
      return setLoadingState(state, "bootstrap", {type: "cleanup", response: null});
    case DELETE_PROVIDER_SUCCESS:
      return setSuccessState(state, "bootstrap", {type: "cleanup", response: action.payload.data});
    case DELETE_PROVIDER_FAILURE:
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "cleanup"});
    case DELETE_PROVIDER_RESPONSE:
      if (action.payload.status === 200) {
        return setSuccessState(state, "bootstrap", {type: "cleanup", response: action.payload.data});
      }
      return setFailureState(state, "bootstrap", action.payload.response.data.error, {type: "cleanup"});

    case RESET_PROVIDER_BOOTSTRAP:
      return setInitialState(state, "bootstrap");

    case LIST_ACCESS_KEYS:
      return setLoadingState(state, "accessKeys", state.accessKeys.data || []);

    case LIST_ACCESS_KEYS_RESPONSE:
      // When we have multiple providers, we would have multiple access keys,
      // we just concat all those into an array.
      if (_.isArray(state.accessKeys.data) &&
          !state.accessKeys.data.find( (a) => (
            action.payload.data.find( (b) => _.isEqual(a.idKey, b.idKey))
          )) && isNonEmptyArray(action.payload.data) ) {
        action.payload.data = state.accessKeys.data.concat(action.payload.data);
      } else {
        action.payload.data = state.accessKeys.data;
      }
      return setPromiseResponse(state, "accessKeys", action);
    case GET_EBS_TYPE_LIST:
      return {...state, ebsTypes: [], status: 'storage', error: null, loading: _.assign(state.loading, {ebsTypes: true})};
    case GET_EBS_TYPE_LIST_RESPONSE:
      if (action.payload.status === 200)
        return { ...state, ebsTypes: action.payload.data, loading: _.assign(state.loading, {ebsTypes: false})};
      return { ...state, ebsTypes: [], error: error, loading: _.assign(state.loading, {ebsTypes: false})};

    case GET_GCP_TYPE_LIST:
      return {...setLoadingState(state, "gcpTypes", [])};
    case GET_GCP_TYPE_LIST_RESPONSE:
      return setPromiseResponse(state, "gcpTypes", action);

    case GET_AZU_TYPE_LIST:
      return {...setLoadingState(state, "azuTypes", [])};
    case GET_AZU_TYPE_LIST_RESPONSE:
      return setPromiseResponse(state, "azuTypes", action);

    case CREATE_DOCKER_PROVIDER:
      return setLoadingState(state, "dockerBootstrap", {});
    case CREATE_DOCKER_PROVIDER_RESPONSE:
      return setPromiseResponse(state, "dockerBootstrap", action);

    case FETCH_CLOUD_METADATA:
      return {...state, fetchMetadata: true};

    case SET_ON_PREM_CONFIG_DATA:
      return {...state, onPremJsonFormData: action.payload};
    case RESET_ON_PREM_CONFIG_DATA:
      return {...state, onPremJsonFormData: {}};

    case GET_NODE_INSTANCE_LIST:
      return setLoadingState(state, "nodeInstanceList", []);
    case GET_NODE_INSTANCE_LIST_RESPONSE:
      return setPromiseResponse(state, "nodeInstanceList", action);

    case EDIT_PROVIDER:
      return setLoadingState(state, "editProvider", {});
    case EDIT_PROVIDER_RESPONSE:
      return setPromiseResponse(state, "editProvider", action);
    case FETCH_AUTH_CONFIG:
      return setLoadingState(state, "authConfig", []);
    case FETCH_AUTH_CONFIG_RESPONSE:
      return setPromiseResponse(state, "authConfig", action);
    case DELETE_KMS_CONFIGURATION:
      return state;
    case DELETE_KMS_CONFIGURATION_RESPONSE:
      // Remove target provider from authConfig list
      const authConfig = state
          .authConfig
          .data
          .filter(val => val.metadata.configUUID !== action.payload);
      state.authConfig['data'] = authConfig;
      return state;
    default:
      return state;
  }
}
