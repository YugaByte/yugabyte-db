// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { isValidObject } from '../../../../utils/ObjectUtils';

export default class YBRadioButton extends Component {
  render() {
    const { input, checkState, fieldValue, label, disabled, isReadOnly, onClick } = this.props;
    let labelClass = this.props.labelClass || 'radio-label';
    if (disabled) {
      labelClass += ' disabled';
    }
    if (isReadOnly) {
      labelClass += ' readonly';
    }
    const name = this.props.name || input.name;
    const id = this.props.id || `radio_button_${name}_${fieldValue}`;
    const onCheckClick = function (event) {
      input && input.onChange && input.onChange(event);
      return isValidObject(onClick) ? onClick(event) : true;
    };
    return (
      <label htmlFor={id} className={labelClass}>
        <input
          {...input}
          type="radio"
          id={id}
          name={name}
          value={fieldValue}
          defaultChecked={checkState}
          disabled={disabled}
          readOnly={isReadOnly}
          onClick={onCheckClick}
        />
        {label}
      </label>
    );
  }
}
