// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';

import './stylesheets/YBLoadingIcon.scss';

export default class YBLoadingLinearIcon extends Component {
  static propTypes = {
    size: PropTypes.oneOf(['xsmall', 'small', 'medium', 'large', 'inline'])
  };

  render() {
    const className =
      'yb-loader-linear' + (this.props.size ? ' yb-loader-linear-' + this.props.size : '');
    return (
      <div className={className}>
        <div></div>
      </div>
    );
  }
}
