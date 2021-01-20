// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';

export default class YBPopover extends Component {
  static propTypes = {
    label: PropTypes.string
  };

  render() {
    const { className, placement, positionTop, positionLeft } = this.props;
    return (
      <div
        className={`popover popover-${placement} ${className}`}
        style={{
          top: positionTop,
          left: positionLeft,
          ...this.props.style
        }}
      >
        {this.props.children}
      </div>
    );
  }
}
