// Copyright (c) YugaByte, Inc.

import axios from 'axios';
import { getCustomerEndpoint } from './common';
export const FETCH_TASK_PROGRESS = 'FETCH_TASK_PROGRESS';
export const FETCH_TASK_PROGRESS_RESPONSE = 'FETCH_TASK_PROGRESS_RESPONSE';
export const RESET_TASK_PROGRESS = 'RESET_TASK_PROGRESS';
export const FETCH_CUSTOMER_TASKS = 'FETCH_CUSTOMER_TASKS';
export const FETCH_CUSTOMER_TASKS_SUCCESS = 'FETCH_CUSTOMER_TASKS_SUCCESS';
export const FETCH_CUSTOMER_TASKS_FAILURE = 'FETCH_CUSTOMER_TASKS_FAILURE';
export const RESET_CUSTOMER_TASKS = 'RESET_CUSTOMER_TASKS';

export const FETCH_FAILED_TASK_DETAIL = 'FETCH_TASK_DETAIL';
export const FETCH_FAILED_TASK_DETAIL_RESPONSE = 'FETCH_TASK_DETAIL_RESPONSE';

export const RETRY_TASK = 'RETRY_TASK';
export const RETRY_TASK_RESPONSE = 'RETRY_TASK_RESPONSE';

export function fetchTaskProgress(taskUUID) {
  const request = axios.get(`${getCustomerEndpoint()}/tasks/${taskUUID}`);
  return {
    type: FETCH_TASK_PROGRESS,
    payload: request
  };
}

export function fetchTaskProgressResponse(result) {
  return {
    type: FETCH_TASK_PROGRESS_RESPONSE,
    payload: result
  };
}

export function resetTaskProgress(error) {
  return {
    type: RESET_TASK_PROGRESS
  };
}

export function fetchCustomerTasks() {
  const request = axios.get(`${getCustomerEndpoint()}/tasks`);
  return {
    type: FETCH_CUSTOMER_TASKS,
    payload: request
  };
}

export function fetchCustomerTasksSuccess(result) {
  return {
    type: FETCH_CUSTOMER_TASKS_SUCCESS,
    payload: result
  };
}

export function fetchCustomerTasksFailure(error) {
  return {
    type: FETCH_CUSTOMER_TASKS_FAILURE,
    payload: error
  };
}

export function resetCustomerTasks() {
  return {
    type: RESET_CUSTOMER_TASKS
  };
}

export function fetchFailedSubTasks(taskUUID) {
  const request = axios.get(`${getCustomerEndpoint()}/tasks/${taskUUID}/failed`);
  return {
    type: FETCH_FAILED_TASK_DETAIL,
    payload: request
  };
}

export function fetchFailedSubTasksResponse(response) {
  return {
    type: FETCH_FAILED_TASK_DETAIL_RESPONSE,
    payload: response.payload
  };
}

export function retryTask(taskUUID) {
  const request = axios.post(`${getCustomerEndpoint()}/tasks/${taskUUID}`);
  return {
    type: RETRY_TASK,
    payload: request
  };
}

export function retryTaskResponse(response) {
  return {
    type: RETRY_TASK_RESPONSE,
    payload: response.payload
  };
}
