// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Link } from 'react-router';

export default class YBBreadcrumb extends Component {
  render() {
    return (
      <span>
        <Link {...this.props}>{this.props.children}</Link>
        <i className="fa fa-angle-right fa-fw"></i>
      </span>
    );
  }
}
