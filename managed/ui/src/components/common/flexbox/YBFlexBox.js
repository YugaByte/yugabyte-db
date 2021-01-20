// Copyright (c) YugaByte, Inc.

import React from 'react';
import './YBFlexBox.scss';

const FlexContainer = ({ className, direction, ...props }) => (
  <div
    className={'flex-cnt ' + className}
    style={{ flexDirection: direction ? direction : 'row' }}
    {...props}
  >
    {props.children}
  </div>
);

const FlexGrow = ({ power, className, ...props }) => (
  <div
    {...props}
    className={className ? 'flex-grow ' + className : 'flex-grow'}
    style={{ ...props.style, flexGrow: power }}
  >
    {props.children}
  </div>
);

const FlexShrink = ({ power, className, ...props }) => (
  <div
    {...props}
    className={className ? 'flex-shrink ' + className : 'flex-shrink'}
    style={{ ...props.style }}
  >
    {props.children}
  </div>
);

export { FlexContainer, FlexGrow, FlexShrink };
