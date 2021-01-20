// Copyright (c) YugaByte, Inc.

import { OPEN_DIALOG, CLOSE_DIALOG } from '../actions/modal';

const INITIAL_STATE = {
  showModal: false,
  visibleModal: ''
};

export default function (state = INITIAL_STATE, action) {
  switch (action.type) {
    // Modal Operations
    case OPEN_DIALOG:
      return { ...state, showModal: true, visibleModal: action.payload };
    case CLOSE_DIALOG:
      return { ...state, showModal: false, visibleModal: '' };

    default:
      return state;
  }
}
