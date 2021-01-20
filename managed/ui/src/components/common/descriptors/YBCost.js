// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import moment from 'moment';
import { YBFormattedNumber } from '../descriptors';

import './stylesheets/YBCost.css';

export default class YBCost extends Component {
  static propTypes = {
    multiplier: PropTypes.oneOf(['day', 'month', 'hour']).isRequired
  };

  render() {
    const { value, multiplier } = this.props;
    let finalCost = value || 0;
    if (multiplier === 'day') {
      finalCost *= 24;
    } else if (multiplier === 'month') {
      finalCost = finalCost * 24 * moment().daysInMonth();
    }
    return (
      <YBFormattedNumber
        value={finalCost}
        maximumFractionDigits={2}
        formattedNumberStyle="currency"
        currency="USD"
        multiplier={multiplier}
      />
    );
  }
}
