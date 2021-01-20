// Copyright (c) YugaByte, Inc.

import axios from 'axios';
import { ROOT_URL } from '../config';

export const CHANGE_GRAPH_QUERY_PERIOD = 'CHANGE_GRAPH_QUERY_PERIOD';
export const RESET_GRAPH_QUERY_PERIOD = 'RESET_GRAPH_QUERY_PERIOD';

export const QUERY_METRICS = 'QUERY_METRICS';
export const QUERY_METRICS_SUCCESS = 'QUERY_METRICS_SUCCESS';
export const QUERY_METRICS_FAILURE = 'QUERY_METRICS_FAILURE';
export const RESET_METRICS = 'RESET_METRICS';

export function changeGraphQueryPeriod(params) {
  return {
    type: CHANGE_GRAPH_QUERY_PERIOD,
    payload: params
  };
}

export function resetGraphQueryPeriod() {
  return {
    type: RESET_GRAPH_QUERY_PERIOD
  };
}

export function queryMetrics(queryParams) {
  const customerUUID = localStorage.getItem('customerId');
  const request = axios.post(`${ROOT_URL}/customers/${customerUUID}/metrics`, queryParams);
  return {
    type: QUERY_METRICS,
    payload: request
  };
}

export function queryMetricsSuccess(result, panelType) {
  return {
    type: QUERY_METRICS_SUCCESS,
    payload: result,
    panelType: panelType
  };
}

export function queryMetricsFailure(error, panelType) {
  return {
    type: QUERY_METRICS_FAILURE,
    payload: error,
    panelType: panelType
  };
}

export function resetMetrics() {
  return {
    type: RESET_METRICS
  };
}
