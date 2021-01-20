// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';

export default class YBCodeBlock extends Component {
  static propTypes = {
    label: PropTypes.string
  };

  render() {
    const { label, className } = this.props;
    return (
      <div className={className ? 'yb-code-block ' + className : 'yb-code-block'}>
        {label && <label>{label}</label>}
        <pre>
          <code>{this.props.children}</code>
        </pre>
      </div>
    );
  }
}
