// Copyright (c) YugaByte, Inc.

import axios from 'axios';
import { ROOT_URL } from '../config';
import { getCustomerEndpoint, getTablesEndpoint, getUniverseEndpoint } from './common';

export const FETCH_TABLES_LIST = 'FETCH_TABLES_LIST';
export const FETCH_TABLES_LIST_SUCCESS = 'FETCH_TABLES_LIST_SUCCESS';
export const FETCH_TABLES_LIST_FAILURE = 'FETCH_TABLES_LIST_FAILURE';
export const RESET_TABLES_LIST = 'RESET_TABLES_LIST';
export const FETCH_TABLE_DETAIL = 'FETCH_TABLE_DETAIL';
export const FETCH_TABLE_DETAIL_SUCCESS = 'FETCH_TABLE_DETAIL_SUCCESS';
export const FETCH_TABLE_DETAIL_FAILURE = 'FETCH_TABLE_DETAIL_FAILURE';
export const RESET_TABLE_DETAIL = 'RESET_TABLE_DETAIL';
export const FETCH_COLUMN_TYPES = 'FETCH_COLUMN_TYPES';
export const FETCH_COLUMN_TYPES_SUCCESS = 'FETCH_COLUMN_TYPES_SUCCESS';
export const FETCH_COLUMN_TYPES_FAILURE = 'FETCH_COLUMN_TYPES_FAILURE';
export const BULK_IMPORT = 'BULK_IMPORT';
export const BULK_IMPORT_RESPONSE = 'BULK_IMPORT_RESPONSE';
export const CREATE_BACKUP_TABLE = 'CREATE_BACKUP_TABLE';
export const CREATE_BACKUP_TABLE_RESPONSE = 'CREATE_BACKUP_TABLE_RESPONSE';
export const RESTORE_TABLE_BACKUP = 'RESTORE_TABLE_BACKUP';
export const RESTORE_TABLE_BACKUP_RESPONSE = 'RESTORE_TABLE_BACKUP_RESPONSE';
export const DELETE_BACKUP = 'DELETE_BACKUP';
export const DELETE_BACKUP_RESPONSE = 'DELETE_BACKUP_RESPONSE';

export function fetchUniverseTables(universeUUID) {
  const customerId = localStorage.getItem('customerId');
  const request = axios.get(`${ROOT_URL}/customers/${customerId}/universes/${universeUUID}/tables`);
  return {
    type: FETCH_TABLES_LIST,
    payload: request
  };
}

export function fetchUniverseTablesSuccess(result) {
  return {
    type: FETCH_TABLES_LIST_SUCCESS,
    payload: result
  };
}

export function fetchUniverseTablesFailure(error) {
  return {
    type: FETCH_TABLES_LIST_FAILURE,
    payload: error
  };
}

export function fetchTableDetail(universeUUID, tableUUID) {
  const customerId = localStorage.getItem('customerId');
  const request = axios.get(
    `${ROOT_URL}/customers/${customerId}/universes/${universeUUID}/tables/${tableUUID}`
  );
  return {
    type: FETCH_TABLE_DETAIL,
    payload: request
  };
}

export function resetTableDetail() {
  return {
    type: RESET_TABLE_DETAIL
  };
}

export function fetchTableDetailSuccess(result) {
  return {
    type: FETCH_TABLE_DETAIL_SUCCESS,
    payload: result
  };
}

export function fetchTableDetailFailure(error) {
  return {
    type: FETCH_TABLE_DETAIL_FAILURE,
    payload: error
  };
}

export function fetchColumnTypes() {
  const request = axios.get(`${ROOT_URL}/metadata/column_types`);
  return {
    type: FETCH_COLUMN_TYPES,
    payload: request
  };
}

export function fetchColumnTypesSuccess(result) {
  return {
    type: FETCH_COLUMN_TYPES_SUCCESS,
    payload: result
  };
}

export function fetchColumnTypesFailure(error) {
  return {
    type: FETCH_COLUMN_TYPES_FAILURE,
    payload: error
  };
}

export function resetTablesList() {
  return {
    type: RESET_TABLES_LIST
  };
}

export function bulkImport(universeUUID, tableUUID, formValues) {
  const customerId = localStorage.getItem('customerId');
  const request = axios.put(
    `${ROOT_URL}/customers/${customerId}/universes/${universeUUID}/tables/${tableUUID}/bulk_import`,
    formValues
  );
  return {
    type: BULK_IMPORT,
    payload: request
  };
}

export function bulkImportResponse(response) {
  return {
    type: BULK_IMPORT_RESPONSE,
    payload: response
  };
}

export function createTableBackup(universeUUID, tableUUID, formValues) {
  const baseUrl = getTablesEndpoint(universeUUID, tableUUID);
  const request = axios.put(`${baseUrl}/create_backup`, formValues);
  return {
    type: CREATE_BACKUP_TABLE,
    payload: request
  };
}

export function createTableBackupResponse(response) {
  return {
    type: CREATE_BACKUP_TABLE_RESPONSE,
    payload: response
  };
}

export function restoreTableBackup(universeUUID, formValues) {
  const baseUrl = getUniverseEndpoint(universeUUID);
  const request = axios.post(`${baseUrl}/backups/restore`, formValues);
  return {
    type: RESTORE_TABLE_BACKUP,
    payload: request
  };
}

export function restoreTableBackupResponse(response) {
  return {
    type: RESTORE_TABLE_BACKUP_RESPONSE,
    payload: response
  };
}

export function deleteBackup(backupUUID) {
  const baseUrl = getCustomerEndpoint();
  const request = axios.delete(`${baseUrl}/backups/${backupUUID}`);
  return {
    type: DELETE_BACKUP,
    payload: request
  };
}

export function deleteBackupResponse(response) {
  return {
    type: DELETE_BACKUP_RESPONSE,
    payload: response
  };
}
