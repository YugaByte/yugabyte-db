// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { ProgressBar } from 'react-bootstrap';

export default class TaskProgressBar extends Component {
  static propTypes = {
    progressData: PropTypes.object.isRequired
  };

  getStyleByStatus(status) {
    if (status === 'Failure') {
      return 'danger';
    } else if (status === 'Success') {
      return 'success';
    } else if (status === 'Running') {
      return 'info';
    }
    return null;
  }

  render() {
    const {
      progressData: { status, percent }
    } = this.props;
    return (
      <ProgressBar now={percent} label={`${percent}%`} bsStyle={this.getStyleByStatus(status)} />
    );
  }
}
