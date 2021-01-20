// Copyright (c) YugaByte, Inc.

import React from 'react';
import { YBButton } from './';

const YBRemoveRowButton = ({ onClick, ...props }) => (
  <YBButton
    btnClass="btn btn-xs yb-remove-button"
    btnIcon="fa fa-remove"
    onClick={onClick}
    {...props}
  />
);

export default YBRemoveRowButton;
