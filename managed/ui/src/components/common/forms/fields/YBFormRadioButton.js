// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';

export default class YBFormRadioButton extends Component {
  render() {
    const {
      field: { name, value, onChange, onBlur },
      id,
      label,
      disabled,
      segmented,
      isReadOnly,
      className,
      ...props
    } = this.props;
    let labelClass = this.props.labelClass || 'radio-label';
    if (segmented) labelClass += ' btn' + (id === value ? ' btn-orange' : ' btn-default');
    if (disabled) {
      labelClass += ' disabled';
    }
    if (isReadOnly) {
      labelClass += ' readonly';
    }
    const key = name + '_' + id;
    return (
      <label htmlFor={key} className={labelClass}>
        <input
          name={name}
          id={key}
          value={id}
          readOnly={isReadOnly}
          disabled={disabled}
          checked={id === value}
          onChange={onChange}
          onBlur={onBlur}
          className={className}
          {...props}
          type="radio"
        />
        {label}
      </label>
    );
  }
}
