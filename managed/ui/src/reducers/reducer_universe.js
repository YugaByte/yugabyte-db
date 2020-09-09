// Copyright (c) YugaByte, Inc.

import { FETCH_UNIVERSE_INFO, RESET_UNIVERSE_INFO, FETCH_UNIVERSE_INFO_RESPONSE, CREATE_UNIVERSE,
  CREATE_UNIVERSE_RESPONSE, EDIT_UNIVERSE, EDIT_UNIVERSE_RESPONSE, FETCH_UNIVERSE_LIST,
  FETCH_UNIVERSE_LIST_RESPONSE, RESET_UNIVERSE_LIST, DELETE_UNIVERSE, DELETE_UNIVERSE_RESPONSE,
  FETCH_UNIVERSE_TASKS, FETCH_UNIVERSE_TASKS_RESPONSE,
  RESET_UNIVERSE_TASKS, CONFIGURE_UNIVERSE_TEMPLATE, CLOSE_UNIVERSE_DIALOG,
  CONFIGURE_UNIVERSE_TEMPLATE_RESPONSE, CONFIGURE_UNIVERSE_TEMPLATE_SUCCESS,
  CONFIGURE_UNIVERSE_TEMPLATE_LOADING,
  CONFIGURE_UNIVERSE_RESOURCES, CONFIGURE_UNIVERSE_RESOURCES_RESPONSE, ROLLING_UPGRADE,
  ROLLING_UPGRADE_RESPONSE, RESET_ROLLING_UPGRADE, SET_UNIVERSE_METRICS, SET_PLACEMENT_STATUS,
  RESET_UNIVERSE_CONFIGURATION, FETCH_UNIVERSE_METADATA, GET_UNIVERSE_PER_NODE_STATUS,
  GET_UNIVERSE_PER_NODE_STATUS_RESPONSE, GET_UNIVERSE_PER_NODE_METRICS,
  GET_UNIVERSE_PER_NODE_METRICS_RESPONSE, GET_MASTER_LEADER, GET_MASTER_LEADER_RESPONSE, RESET_MASTER_LEADER,
  PERFORM_UNIVERSE_NODE_ACTION, PERFORM_UNIVERSE_NODE_ACTION_RESPONSE, FETCH_UNIVERSE_BACKUPS,
  FETCH_UNIVERSE_BACKUPS_RESPONSE, RESET_UNIVERSE_BACKUPS,
  CREATE_UNIVERSE_BACKUP, CREATE_UNIVERSE_BACKUP_RESPONSE, GET_HEALTH_CHECK,
  GET_HEALTH_CHECK_RESPONSE, ADD_READ_REPLICA, ADD_READ_REPLICA_RESPONSE, EDIT_READ_REPLICA,
  EDIT_READ_REPLICA_RESPONSE, DELETE_READ_REPLICA, DELETE_READ_REPLICA_RESPONSE,
  IMPORT_UNIVERSE, IMPORT_UNIVERSE_RESPONSE, IMPORT_UNIVERSE_RESET, IMPORT_UNIVERSE_INIT,
  UPDATE_BACKUP_STATE, UPDATE_BACKUP_STATE_RESPONSE, SET_ALERTS_CONFIG, SET_ALERTS_CONFIG_RESPONSE
} from '../actions/universe';
import _ from 'lodash';
import {
  getInitialState,
  setInitialState,
  setLoadingState,
  setPromiseResponse,
  setSuccessState
} from '../utils/PromiseUtils.js';
import { isNonEmptyArray, isNonEmptyObject } from '../utils/ObjectUtils.js';

const INITIAL_STATE = {
  currentUniverse: getInitialState({}),
  createUniverse: getInitialState({}),
  editUniverse: getInitialState({}),
  deleteUniverse: getInitialState({}),
  universeList: getInitialState([]),
  error: null,
  formSubmitSuccess: false,
  universeConfigTemplate: getInitialState({}),
  universeResourceTemplate: getInitialState({}),
  currentPlacementStatus: null,
  fetchUniverseMetadata: false,
  addReadReplica: getInitialState([]),
  editReadReplica: getInitialState([]),
  deleteReadReplica: getInitialState([]),
  universeTasks: getInitialState([]),
  universePerNodeStatus: getInitialState({}),
  universePerNodeMetrics: getInitialState({}),
  universeMasterLeader: getInitialState({}),
  rollingUpgrade: getInitialState({}),
  universeNodeAction: getInitialState({}),
  createUniverseBackup: getInitialState({}),
  universeBackupList: getInitialState({}),
  healthCheck: getInitialState({}),
  universeImport: getInitialState({}),
  alertsConfig: getInitialState({}),
  backupState: getInitialState({})
};

export default function(state = INITIAL_STATE, action) {
  switch(action.type) {

    // Universe CRUD Operations
    case CREATE_UNIVERSE:
      return setLoadingState(state, "createUniverse", {});
    case CREATE_UNIVERSE_RESPONSE:
      return setPromiseResponse(state, "createUniverse", action);
    case EDIT_UNIVERSE:
      return setLoadingState(state, "editUniverse", {});
    case EDIT_UNIVERSE_RESPONSE:
      return setPromiseResponse(state, "editUniverse", action);
    case DELETE_UNIVERSE:
      return setLoadingState(state, "deleteUniverse", {});
    case DELETE_UNIVERSE_RESPONSE:
      return setPromiseResponse(state, "deleteUniverse", action);

    // Co-Modal Operations
    case CLOSE_UNIVERSE_DIALOG:
      return { ...state, universeConfigTemplate: getInitialState({}), universeResourceTemplate: getInitialState({})};

    // Read Replica Operations
    case ADD_READ_REPLICA:
      return setLoadingState(state, "addReadReplica", {});
    case ADD_READ_REPLICA_RESPONSE:
      return setPromiseResponse(state, "addReadReplica", action);
    case EDIT_READ_REPLICA:
      return setLoadingState(state, "editReadReplica", {});
    case EDIT_READ_REPLICA_RESPONSE:
      return setPromiseResponse(state, "editReadReplica", action);
    case DELETE_READ_REPLICA:
      return setLoadingState(state, "deleteReadReplica", {});
    case DELETE_READ_REPLICA_RESPONSE:
      return setPromiseResponse(state, "deleteReadReplica", action);

    // Universe GET operations
    case FETCH_UNIVERSE_INFO:
      return setLoadingState(state, "currentUniverse", {});
    case FETCH_UNIVERSE_INFO_RESPONSE:
      return setPromiseResponse(state, "currentUniverse", action);
    case RESET_UNIVERSE_INFO:
      return { ...state, currentUniverse: getInitialState({})};
    case FETCH_UNIVERSE_LIST:
      return setLoadingState(state, "universeList", []);
    case FETCH_UNIVERSE_LIST_RESPONSE:
      return {...setPromiseResponse(state, "universeList", action), fetchUniverseMetadata: false};
    case RESET_UNIVERSE_LIST:
      return { ...state, universeList: getInitialState([]), universeCurrentCostList: [], currentTotalCost: 0, error: null};
    case GET_UNIVERSE_PER_NODE_STATUS:
      return setLoadingState(state, "universePerNodeStatus", {});
    case GET_UNIVERSE_PER_NODE_STATUS_RESPONSE:
      return setPromiseResponse(state, "universePerNodeStatus", action);
    case GET_UNIVERSE_PER_NODE_METRICS:
      return setLoadingState(state, "universePerNodeMetrics", {});
    case GET_UNIVERSE_PER_NODE_METRICS_RESPONSE:
      return setPromiseResponse(state, "universePerNodeMetrics", action);
    case GET_MASTER_LEADER:
      return setLoadingState(state, "universeMasterLeader", {});
    case GET_MASTER_LEADER_RESPONSE:
      return setPromiseResponse(state, "universeMasterLeader", action);
    case RESET_MASTER_LEADER:
      return { ...state, universeMasterLeader: getInitialState({})};

    // Universe Tasks Operations
    case FETCH_UNIVERSE_TASKS:
      return setLoadingState(state, "universeTasks", []);
    case FETCH_UNIVERSE_TASKS_RESPONSE:
      return setPromiseResponse(state, "universeTasks", action);
    case RESET_UNIVERSE_TASKS:
      return { ...state, universeTasks: getInitialState([])};

    // Universe Configure Operations
    case CONFIGURE_UNIVERSE_TEMPLATE:
      return setLoadingState(state, "universeConfigTemplate", {});
    case CONFIGURE_UNIVERSE_TEMPLATE_RESPONSE:
      return setPromiseResponse(state, "universeConfigTemplate", action);
    case CONFIGURE_UNIVERSE_TEMPLATE_SUCCESS:
      return setSuccessState(state, "universeConfigTemplate", action.payload.data);
    case CONFIGURE_UNIVERSE_TEMPLATE_LOADING:
      return setLoadingState(state, "universeConfigTemplate");

    case CONFIGURE_UNIVERSE_RESOURCES:
      return setLoadingState(state, "universeResourceTemplate", {});
    case CONFIGURE_UNIVERSE_RESOURCES_RESPONSE:
      return setPromiseResponse(state, "universeResourceTemplate", action);

    // Universe Rolling Upgrade Operations
    case ROLLING_UPGRADE:
      return setLoadingState(state, "rollingUpgrade", {});
    case ROLLING_UPGRADE_RESPONSE:
      return setPromiseResponse(state, "rollingUpgrade", action);
    case RESET_ROLLING_UPGRADE:
      return { ...state, error: null, "rollingUpgrade": getInitialState({})};

    // Universe I/O Metrics Operations
    case SET_UNIVERSE_METRICS:
      const currentUniverseList = _.clone(state.universeList.data, true);
      if (isNonEmptyObject(action.payload.data.tserver_rpcs_per_sec_by_universe)) {
        const universeReadWriteMetricList = action.payload.data.tserver_rpcs_per_sec_by_universe.data;
        isNonEmptyArray(universeReadWriteMetricList) &&
        universeReadWriteMetricList.forEach(function(metricData){
          for(let counter = 0; counter < currentUniverseList.length; counter++) {
            const nodePrefix = currentUniverseList[counter].universeDetails.nodePrefix;
            if (nodePrefix && nodePrefix.trim() === metricData.name.trim()) {
              if (metricData.labels["service_method"] === "Read") {
                currentUniverseList[counter]["readData"] = metricData;
              } else if (metricData.labels["service_method"] === "Write") {
                currentUniverseList[counter]["writeData"] = metricData;
              }
            }
          }
        });
      }
      return setSuccessState(state, "universeList", currentUniverseList);

    case SET_PLACEMENT_STATUS:
      return {...state, currentPlacementStatus: action.payload};
    case RESET_UNIVERSE_CONFIGURATION:
      return {...state, currentPlacementStatus: null, universeResourceTemplate: getInitialState({}), universeConfigTemplate: getInitialState({})};
    case FETCH_UNIVERSE_METADATA:
      return {...state, fetchUniverseMetadata: true};

    case PERFORM_UNIVERSE_NODE_ACTION:
      return setLoadingState(state, "universeNodeAction", {});
    case PERFORM_UNIVERSE_NODE_ACTION_RESPONSE:
      return setPromiseResponse(state, "universeNodeAction", action);

    case FETCH_UNIVERSE_BACKUPS:
      return setLoadingState(state, "universeBackupList", {});
    case FETCH_UNIVERSE_BACKUPS_RESPONSE:
      return setPromiseResponse(state, "universeBackupList", action);
    case RESET_UNIVERSE_BACKUPS:
      return setInitialState(state, 'universeBackupList', []);

    case CREATE_UNIVERSE_BACKUP:
      return setLoadingState(state, "createUniverseBackup", {});
    case CREATE_UNIVERSE_BACKUP_RESPONSE:
      return setPromiseResponse(state, "createUniverseBackup", action);

    // Universe Health Checking
    case GET_HEALTH_CHECK:
      return setLoadingState(state, "healthCheck", []);
    case GET_HEALTH_CHECK_RESPONSE:
      return setPromiseResponse(state, "healthCheck", action);

    // Universe import
    case IMPORT_UNIVERSE:
      return setLoadingState(state, "universeImport", []);
    case IMPORT_UNIVERSE_INIT:
      return setLoadingState(state, "universeImport", []);
    case IMPORT_UNIVERSE_RESET:
      return { ...state, universeImport: getInitialState([])};
    case IMPORT_UNIVERSE_RESPONSE:
      return setPromiseResponse(state, "universeImport", action);
    case SET_ALERTS_CONFIG:
      return { ...state, alertsConfig: getInitialState([])};
    case SET_ALERTS_CONFIG_RESPONSE:
      return setPromiseResponse(state, "alertsConfig", action);
    case UPDATE_BACKUP_STATE:
      return { ...state, backupState: getInitialState([])};
    case UPDATE_BACKUP_STATE_RESPONSE:
      return setPromiseResponse(state, "backupState", action);

    default:
      return state;
  }
}
