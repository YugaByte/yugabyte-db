// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';

export default class YBLabelWithIcon extends Component {
  static propTypes = {
    icon: PropTypes.string
  };
  render() {
    const { icon } = this.props;
    return (
      <span>
        {icon && <i className={icon}></i>}
        {this.props.children}
      </span>
    );
  }
}
