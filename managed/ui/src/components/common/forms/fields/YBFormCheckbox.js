// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';

export default class YBFormCheckbox extends Component {
  render() {
    const {
      field: { name, value, onChange, onBlur },
      form: { errors, touched, setFieldValue },
      id,
      label,
      disabled,
      segmented,
      isReadOnly,
      className,
      ...props
    } = this.props;
    let labelClass = this.props.labelClass || 'checkbox-label';
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
          checked={value}
          onChange={onChange}
          onBlur={onBlur}
          className={className}
          type="checkbox"
          {...props}
        />
        {label}
      </label>
    );
  }
}
