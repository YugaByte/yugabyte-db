// Copyright (c) YugaByte, Inc.
import { isNonEmptyArray, isNullOrEmpty, isDefinedNotNull } from '../utils/ObjectUtils';
import _ from 'lodash';
import { Enum } from 'enumify';

class PromiseState extends Enum {
  isSuccess() {
    return this === PromiseState.SUCCESS;
  }
  isError() {
    return this === PromiseState.ERROR;
  }
  isEmpty() {
    return this === PromiseState.EMPTY;
  }
  isLoading() {
    return this === PromiseState.LOADING;
  }
  isInit() {
    return this === PromiseState.INIT;
  }
}

PromiseState.initEnum(['INIT', 'SUCCESS', 'LOADING', 'ERROR', 'EMPTY']);

function setPromiseState(state, object, promiseState, data = null, error = null) {
  return Object.assign({}, state, {
    [object]: { data: data, promiseState: promiseState, error: error }
  });
}

export function setLoadingState(state, object, data = null) {
  if (data) {
    return setPromiseState(state, object, PromiseState.LOADING, data);
  } else {
    return Object.assign({}, state, {
      [object]: Object.assign({}, state[object], {
        promiseState: PromiseState.LOADING
      })
    });
  }
}

export function setSuccessState(state, object, data) {
  return setPromiseState(state, object, PromiseState.SUCCESS, data);
}

export function setFailureState(state, object, error, data = null) {
  return setPromiseState(state, object, PromiseState.ERROR, data, error);
}

export function getInitialState(data = null) {
  return { data: data, promiseState: PromiseState.INIT, error: null };
}

export function setInitialState(state, object, data = {}, error = null) {
  return setPromiseState(state, object, PromiseState.INIT, data, error);
}

export function getPromiseState(dataObject) {
  if (
    isDefinedNotNull(dataObject.data) &&
    (isNonEmptyArray(dataObject.data) || !isNullOrEmpty(dataObject.data))
  ) {
    if (dataObject.promiseState === PromiseState.LOADING) return PromiseState.LOADING;
    return PromiseState.SUCCESS;
  } else if (isDefinedNotNull(dataObject.promiseState) && dataObject.promiseState.isSuccess()) {
    return PromiseState.EMPTY;
  } else {
    return dataObject.promiseState;
  }
}

export function setPromiseResponse(state, object, response) {
  const {
    payload,
    payload: { data, status, isAxiosError }
  } = response;
  const objectState = _.omit(response, ['payload', 'type']);
  if (status !== 200 || isAxiosError || (isDefinedNotNull(data) && isDefinedNotNull(data.error))) {
    _.merge(objectState, {
      data: null,
      error:
        isDefinedNotNull(payload.response) && payload.response.data && payload.response.data.error,
      promiseState: PromiseState.ERROR
    });
  } else {
    _.merge(objectState, { data: data, error: null, promiseState: PromiseState.SUCCESS });
  }
  return Object.assign({}, state, { [object]: objectState });
}
