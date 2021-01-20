// Copyright (c) YugaByte, Inc.

import { createStore, applyMiddleware } from 'redux';
import rootReducer from '../reducers';
import promise from 'redux-promise';

// Middleware you want to use in production:
const enhancer = applyMiddleware(promise);

export default function configureStore(initialState) {
  // Note: only Redux >= 3.1.0 supports passing enhancer as third argument.
  // See https://github.com/rackt/redux/releases/tag/v3.1.0
  return createStore(rootReducer, initialState, enhancer);
}
