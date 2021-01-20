// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { YBLabel } from '../../../../components/common/descriptors';
import DayPickerInput from 'react-day-picker/DayPickerInput';
import 'react-day-picker/lib/style.css';

export default class YBFormDatePicker extends Component {
  render() {
    const { pickerComponent } = this.props;
    return (
      <YBLabel {...this.props}>
        <DayPickerInput component={pickerComponent} {...this.props} {...this.props.field} />
      </YBLabel>
    );
  }
}
