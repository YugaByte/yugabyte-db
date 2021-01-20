// Copyright (c) YugaByte, Inc.

import {
  FETCH_TABLES_LIST,
  FETCH_TABLES_LIST_SUCCESS,
  FETCH_TABLES_LIST_FAILURE,
  RESET_TABLES_LIST,
  FETCH_TABLE_DETAIL,
  FETCH_TABLE_DETAIL_SUCCESS,
  FETCH_TABLE_DETAIL_FAILURE,
  RESET_TABLE_DETAIL,
  FETCH_COLUMN_TYPES,
  FETCH_COLUMN_TYPES_SUCCESS,
  FETCH_COLUMN_TYPES_FAILURE,
  BULK_IMPORT,
  BULK_IMPORT_RESPONSE,
  CREATE_BACKUP_TABLE,
  CREATE_BACKUP_TABLE_RESPONSE,
  RESTORE_TABLE_BACKUP,
  RESTORE_TABLE_BACKUP_RESPONSE
} from '../actions/tables';
import { getInitialState, setLoadingState, setPromiseResponse } from '../utils/PromiseUtils';

const INITIAL_STATE = {
  universeTablesList: [],
  currentTableDetail: {},
  columnDataTypes: {},
  currentTableView: 'list',
  bulkImport: getInitialState({}),
  createBackup: getInitialState({}),
  restoreBackup: getInitialState({})
};

export default function (state = INITIAL_STATE, action) {
  let error;
  switch (action.type) {
    case FETCH_TABLES_LIST:
      return { ...state, universeTablesList: [], loading: true };
    case FETCH_TABLES_LIST_SUCCESS:
      if (Array.isArray(action.payload.data)) {
        return { ...state, universeTablesList: action.payload.data, error: null, loading: false };
      } else {
        return { ...state, universeTablesList: [], error: null, loading: false };
      }
    case FETCH_TABLES_LIST_FAILURE:
      error = action.payload.data || { message: action.payload.response.data.error };
      return { ...state, universeTablesList: [], error: error, loading: false };
    case RESET_TABLES_LIST:
      return { ...state, universeTablesList: [], error: null, loading: false };
    case FETCH_TABLE_DETAIL:
      return { ...state, currentTableDetail: {}, loading: true, error: null };
    case FETCH_TABLE_DETAIL_SUCCESS:
      return { ...state, currentTableDetail: action.payload.data, loading: false, error: null };
    case FETCH_TABLE_DETAIL_FAILURE:
      return {
        ...state,
        currentTableDetail: {},
        loading: false,
        error: action.payload.response.data.error
      };
    case RESET_TABLE_DETAIL:
      return { ...state, currentTableDetail: {} };
    case FETCH_COLUMN_TYPES:
      return { ...state };
    case FETCH_COLUMN_TYPES_SUCCESS:
      return { ...state, columnDataTypes: action.payload.data };
    case FETCH_COLUMN_TYPES_FAILURE:
      return { ...state };
    case BULK_IMPORT:
      return setLoadingState(state, 'bulkImport', {});
    case BULK_IMPORT_RESPONSE:
      return setPromiseResponse(state, 'bulkImport', action);
    case CREATE_BACKUP_TABLE:
      return setLoadingState(state, 'createBackup', {});
    case CREATE_BACKUP_TABLE_RESPONSE:
      return setPromiseResponse(state, 'createBackup', action);
    case RESTORE_TABLE_BACKUP:
      return setLoadingState(state, 'restoreBackup', {});
    case RESTORE_TABLE_BACKUP_RESPONSE:
      return setPromiseResponse(state, 'restoreBackup', action);
    default:
      return state;
  }
}
