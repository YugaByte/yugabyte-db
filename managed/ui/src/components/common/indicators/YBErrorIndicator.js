// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import frown from './images/frown_img.png';
import './stylesheets/YBErrorIndicator.scss';
import { Link } from 'react-router';

export default class YBErrorIndicator extends Component {
  render() {
    const { type, uuid } = this.props;
    let errorDisplayMessage = <span />;
    let errorRecoveryMessage = <span />;

    if (type === 'universe') {
      errorDisplayMessage = (
        <div>Seems like universe with UUID {uuid} has issues or does not exist.</div>
      );
      errorRecoveryMessage = (
        <div>
          Click <Link to={'/'}>here</Link> to go back to home page.
        </div>
      );
    }
    return (
      <div className="yb-error-container">
        <div>
          Aww Snap. <img src={frown} className="yb-sad-face" alt="sad face" />
        </div>
        <div>{errorDisplayMessage}</div>
        <div>{errorRecoveryMessage}</div>
      </div>
    );
  }
}
