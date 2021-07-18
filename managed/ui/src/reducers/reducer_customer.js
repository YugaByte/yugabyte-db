// Copyright (c) YugaByte, Inc.

import _ from 'lodash';
import {
  VALIDATE_FROM_TOKEN,
  VALIDATE_FROM_TOKEN_RESPONSE,
  REGISTER,
  REGISTER_RESPONSE,
  LOGIN,
  LOGIN_RESPONSE,
  INSECURE_LOGIN,
  INSECURE_LOGIN_RESPONSE,
  INVALID_CUSTOMER_TOKEN,
  RESET_TOKEN_ERROR,
  RESET_CUSTOMER,
  LOGOUT,
  LOGOUT_SUCCESS,
  LOGOUT_FAILURE,
  FETCH_SOFTWARE_VERSIONS_FAILURE,
  FETCH_SOFTWARE_VERSIONS_SUCCESS,
  FETCH_SOFTWARE_VERSIONS,
  FETCH_TLS_CERTS,
  FETCH_TLS_CERTS_RESPONSE,
  ADD_TLS_CERT,
  ADD_TLS_CERT_RESPONSE,
  ADD_TLS_CERT_RESET,
  FETCH_HOST_INFO,
  FETCH_HOST_INFO_SUCCESS,
  FETCH_HOST_INFO_FAILURE,
  FETCH_CUSTOMER_COUNT,
  FETCH_YUGAWARE_VERSION,
  FETCH_YUGAWARE_VERSION_RESPONSE,
  UPDATE_PROFILE,
  UPDATE_PROFILE_SUCCESS,
  UPDATE_PROFILE_FAILURE,
  ADD_CUSTOMER_CONFIG,
  ADD_CUSTOMER_CONFIG_RESPONSE,
  SET_INITIAL_VALUES,
  EDIT_CUSTOMER_CONFIG,
  EDIT_CUSTOMER_CONFIG_RESPONSE,
  FETCH_CUSTOMER_CONFIGS,
  FETCH_CUSTOMER_CONFIGS_RESPONSE,
  DELETE_CUSTOMER_CONFIG,
  DELETE_CUSTOMER_CONFIG_RESPONSE,
  GET_LOGS,
  GET_LOGS_SUCCESS,
  GET_LOGS_FAILURE,
  GET_RELEASES,
  GET_RELEASES_RESPONSE,
  REFRESH_RELEASES,
  REFRESH_RELEASES_RESPONSE,
  IMPORT_RELEASE,
  IMPORT_RELEASE_RESPONSE,
  UPDATE_RELEASE,
  UPDATE_RELEASE_RESPONSE,
  GET_ALERTS,
  GET_ALERTS_SUCCESS,
  GET_ALERTS_FAILURE,
  API_TOKEN_LOADING,
  API_TOKEN,
  API_TOKEN_RESPONSE,
  GET_SCHEDULES,
  GET_SCHEDULES_RESPONSE,
  DELETE_SCHEDULE,
  DELETE_SCHEDULE_RESPONSE,
  GET_CUSTOMER_USERS,
  GET_CUSTOMER_USERS_SUCCESS,
  GET_CUSTOMER_USERS_FAILURE,
  CREATE_USER,
  CREATE_USER_RESPONSE,
  CREATE_ALERT_RECEIVER,
  CREATE_ALERT_RECEIVER_RESPONSE,
  GET_ALERT_RECEIVERS,
  GET_ALERT_DESTIONATIONS,
  GET_ALERT_DEFINATION_TEMPLATES,
  GET_ALERT_CONFIGS,
  CREATE_ALERT_DESTINATION,
  CREATE_ALERT_DESTINATION_RESPONSE,
  CREATE_ALERT_CONFIG,
  CREATE_ALERT_CONFIG_RESPONSE,
  UPDATE_ALERT_DESTINATION,
  UPDATE_ALERT_DESTINATION_RESPONSE,
  UPDATE_ALERT_CONFIG,
  UPDATE_ALERT_CONFIG_RESPONSE,
  DELETE_ALERT_DESTINATION,
  DELETE_ALERT_CONFIG
} from '../actions/customers';

import { sortVersionStrings, isDefinedNotNull } from '../utils/ObjectUtils';
import {
  getInitialState,
  setLoadingState,
  setSuccessState,
  setFailureState,
  setPromiseResponse
} from '../utils/PromiseUtils';

const INITIAL_STATE = {
  currentCustomer: getInitialState({}),
  authToken: getInitialState({}),
  apiToken: getInitialState(null),
  tasks: [],
  status: null,
  error: null,
  loading: false,
  softwareVersions: [],
  alerts: {
    alertsList: [],
    updated: null
  },
  alertReceivers: getInitialState([]),
  alertDestinations: getInitialState([]),
  alertTemplates: getInitialState([]),
  alertConfigs: getInitialState([]),
  deleteDestination: getInitialState([]),
  deleteAlertConfig: getInitialState([]),
  hostInfo: null,
  customerCount: {},
  yugawareVersion: getInitialState({}),
  profile: getInitialState({}),
  addConfig: getInitialState({}),
  setInitialVal: getInitialState({}),
  editConfig: getInitialState({}),
  configs: getInitialState([]),
  deleteConfig: getInitialState({}),
  deleteSchedule: getInitialState({}),
  releases: getInitialState([]),
  refreshReleases: getInitialState({}),
  importRelease: getInitialState({}),
  updateRelease: getInitialState({}),
  addCertificate: getInitialState({}),
  userCertificates: getInitialState({}),
  users: getInitialState([]),
  schedules: getInitialState([]),
  createUser: getInitialState({}),
  createAlertReceiver: getInitialState({}),
  createAlertDestination: getInitialState({}),
  createAlertConfig: getInitialState({}),
  updateAlertDestination: getInitialState({}),
  updateAlertConfig: getInitialState({})
};

export default function (state = INITIAL_STATE, action) {
  switch (action.type) {
    case VALIDATE_FROM_TOKEN:
      return setLoadingState(state, 'currentCustomer', {});
    case VALIDATE_FROM_TOKEN_RESPONSE:
      return setPromiseResponse(state, 'currentCustomer', action);
    case REGISTER:
      return setLoadingState(state, 'authToken', {});
    case REGISTER_RESPONSE:
      return setPromiseResponse(state, 'authToken', action);

    case LOGIN:
      return setLoadingState(state, 'authToken', {});
    case LOGIN_RESPONSE:
      return setPromiseResponse(state, 'authToken', action);

    case API_TOKEN_LOADING:
      return setLoadingState(state, 'apiToken', null);
    case API_TOKEN:
      return setLoadingState(state, 'apiToken', null);
    case API_TOKEN_RESPONSE:
      return setPromiseResponse(state, 'apiToken', action);

    case INSECURE_LOGIN:
      return {
        ...state,
        INSECURE_apiToken: null
      };
    case INSECURE_LOGIN_RESPONSE:
      return {
        ...state,
        INSECURE_apiToken: action.payload.data.apiToken
      };
    case LOGOUT:
      return { ...state };
    case LOGOUT_SUCCESS:
      return { ...state, currentCustomer: getInitialState({}), authToken: getInitialState({}) };
    case LOGOUT_FAILURE:
      return { ...state };
    case INVALID_CUSTOMER_TOKEN:
      return { ...state, error: 'Invalid' };
    case RESET_TOKEN_ERROR:
      return { ...state, error: null };
    case RESET_CUSTOMER:
      return { ...state, currentCustomer: getInitialState({}), authToken: getInitialState({}) };
    case FETCH_SOFTWARE_VERSIONS:
      return { ...state, softwareVersions: [] };
    case FETCH_SOFTWARE_VERSIONS_SUCCESS:
      return { ...state, softwareVersions: sortVersionStrings(action.payload.data) };
    case FETCH_SOFTWARE_VERSIONS_FAILURE:
      return { ...state };
    case FETCH_TLS_CERTS:
      return setLoadingState(state, 'userCertificates', []);
    case FETCH_TLS_CERTS_RESPONSE:
      return setPromiseResponse(state, 'userCertificates', action);
    case ADD_TLS_CERT:
      return setLoadingState(state, 'addCertificate', {});
    case ADD_TLS_CERT_RESPONSE:
      if (action.payload.status !== 200) {
        if (isDefinedNotNull(action.payload.data)) {
          return setFailureState(state, 'addCertificate', action.payload.response.data.error);
        } else {
          return state;
        }
      }
      return setPromiseResponse(state, 'addCertificate', action);
    case ADD_TLS_CERT_RESET:
      return setLoadingState(state, 'addCertificate', getInitialState({}));
    case FETCH_HOST_INFO:
      return { ...state, hostInfo: null };
    case FETCH_HOST_INFO_SUCCESS:
      return { ...state, hostInfo: action.payload.data };
    case FETCH_HOST_INFO_FAILURE:
      return { ...state, hostInfo: null };

    case UPDATE_PROFILE:
      return setLoadingState(state, 'profile');
    case UPDATE_PROFILE_SUCCESS:
      return setSuccessState(state, 'profile', 'updated-success');
    case UPDATE_PROFILE_FAILURE:
      return setFailureState(state, 'profile', action.payload.response.data.error);
    case FETCH_CUSTOMER_COUNT:
      return setLoadingState(state, 'customerCount');
    case GET_ALERTS:
      return {
        ...state,
        alerts: {
          alertsList: [],
          updated: null
        }
      };
    case GET_ALERTS_SUCCESS:
      return {
        ...state,
        alerts: {
          alertsList: action.payload.data,
          updated: Date.now()
        }
      };
    case GET_ALERTS_FAILURE:
      return {
        ...state,
        alerts: {
          alertsList: [],
          updated: Date.now()
        }
      };
    case GET_ALERT_RECEIVERS:
      return setLoadingState(state, 'alertReceivers', []);
    case GET_ALERT_DESTIONATIONS:
      return setLoadingState(state, 'alertDestinations', []);
    case GET_ALERT_DEFINATION_TEMPLATES:
      return setLoadingState(state, 'alertTemplates', []);
    case GET_ALERT_CONFIGS:
      return setLoadingState(state, 'alertConfigs', []);
    case DELETE_ALERT_DESTINATION:
      return setLoadingState(state, 'deleteDestination', []);
    case DELETE_ALERT_CONFIG:
      return setLoadingState(state, 'deleteAlertConfig', []);
    case CREATE_ALERT_RECEIVER:
      return setLoadingState(state, 'createAlertReceiver', {});
    case CREATE_ALERT_RECEIVER_RESPONSE:
      if (action.payload.status !== 200) {
        if (isDefinedNotNull(action.payload.data)) {
          return setFailureState(state, 'createAlertReceiver', action.payload.response.data.error);
        } else {
          return state;
        }
      }
      return setPromiseResponse(state, 'createAlertReceiver', action);
    case CREATE_ALERT_DESTINATION:
      return setLoadingState(state, 'createAlertDestination', {});
    case CREATE_ALERT_DESTINATION_RESPONSE:
      if (action.payload.status !== 200) {
        if (isDefinedNotNull(action.payload.data)) {
          return setFailureState(state, 'createAlertReceiver', action.payload.response.data.error);
        } else {
          return state;
        }
      }
      return setPromiseResponse(state, 'createAlertReceiver', action);
    case CREATE_ALERT_CONFIG:
      return setLoadingState(state, 'createAlertConfig', {});
    case CREATE_ALERT_CONFIG_RESPONSE:
      if (action.payload.status !== 200) {
        if (isDefinedNotNull(action.payload.data)) {
          return setFailureState(state, 'createAlertReceiver', action.payload.response.data.error);
        } else {
          return state;
        }
      }
      return setPromiseResponse(state, 'createAlertReceiver', action);
    case UPDATE_ALERT_DESTINATION:
      return setLoadingState(state, 'updateAlertDestination', {});
    case UPDATE_ALERT_DESTINATION_RESPONSE:
      if (action.payload.status !== 200) {
        if (isDefinedNotNull(action.payload.data)) {
          return setFailureState(state, 'createAlertReceiver', action.payload.response.data.error);
        } else {
          return state;
        }
      }
      return setPromiseResponse(state, 'createAlertReceiver', action);
    case UPDATE_ALERT_CONFIG:
      return setLoadingState(state, 'updateAlertConfig', {});
    case UPDATE_ALERT_CONFIG_RESPONSE:
      if (action.payload.status !== 200) {
        if (isDefinedNotNull(action.payload.data)) {
          return setFailureState(state, 'createAlertReceiver', action.payload.response.data.error);
        } else {
          return state;
        }
      }
      return setPromiseResponse(state, 'createAlertReceiver', action);
    case FETCH_YUGAWARE_VERSION:
      return setLoadingState(state, 'yugawareVersion', {});
    case FETCH_YUGAWARE_VERSION_RESPONSE:
      return setPromiseResponse(state, 'yugawareVersion', action);
    case ADD_CUSTOMER_CONFIG:
      return setLoadingState(state, 'addConfig', {});
    case SET_INITIAL_VALUES:
      return {
        ...state,
        setInitialVal: action.payload
      };
    case ADD_CUSTOMER_CONFIG_RESPONSE:
      return setPromiseResponse(state, 'addConfig', action);
    case EDIT_CUSTOMER_CONFIG:
      return setLoadingState(state, 'editConfig', {});
    case EDIT_CUSTOMER_CONFIG_RESPONSE:
      return setPromiseResponse(state, 'editConfig', action);
    case FETCH_CUSTOMER_CONFIGS:
      return setLoadingState(state, 'configs', []);
    case FETCH_CUSTOMER_CONFIGS_RESPONSE:
      return setPromiseResponse(state, 'configs', action);
    case DELETE_CUSTOMER_CONFIG:
      return setLoadingState(state, 'deleteConfig', {});
    case DELETE_CUSTOMER_CONFIG_RESPONSE:
      return setPromiseResponse(state, 'deleteConfig', action);

    case GET_LOGS:
      return {
        ...state,
        yugaware_logs: null
      };
    case GET_LOGS_SUCCESS:
      return {
        ...state,
        yugaware_logs: action.payload.data.lines.reverse(),
        yugawareLogError: false
      };
    case GET_LOGS_FAILURE:
      return {
        ...state,
        yugaware_logs: null,
        yugawareLogError: true
      };

    case GET_CUSTOMER_USERS:
      return setLoadingState(state, 'users', getInitialState([]));
    case GET_CUSTOMER_USERS_SUCCESS:
      return setSuccessState(state, 'users', _.sortBy(action.payload.data, 'creationDate'));
    case GET_CUSTOMER_USERS_FAILURE:
      return setFailureState(state, 'users', action.payload);

    case CREATE_USER:
      return setLoadingState(state, 'createUser', {});
    case CREATE_USER_RESPONSE:
      return setPromiseResponse(state, 'createUser', action);

    case GET_RELEASES:
      return setLoadingState(state, 'releases', []);
    case GET_RELEASES_RESPONSE:
      return setPromiseResponse(state, 'releases', action);
    case REFRESH_RELEASES:
      return setLoadingState(state, 'refreshReleases', {});
    case REFRESH_RELEASES_RESPONSE:
      return setPromiseResponse(state, 'refreshReleases', action);
    case IMPORT_RELEASE:
      return setLoadingState(state, 'importRelease', {});
    case IMPORT_RELEASE_RESPONSE:
      return setPromiseResponse(state, 'importRelease', action);
    case UPDATE_RELEASE:
      return setLoadingState(state, 'updateRelease', {});
    case UPDATE_RELEASE_RESPONSE:
      return setPromiseResponse(state, 'updateRelease', action);
    case GET_SCHEDULES:
      return setLoadingState(state, 'schedules', []);
    case GET_SCHEDULES_RESPONSE:
      return setPromiseResponse(state, 'schedules', action);
    case DELETE_SCHEDULE:
      return setLoadingState(state, 'deleteSchedule', {});
    case DELETE_SCHEDULE_RESPONSE:
      return setPromiseResponse(state, 'deleteSchedule', action);

    default:
      return state;
  }
}
