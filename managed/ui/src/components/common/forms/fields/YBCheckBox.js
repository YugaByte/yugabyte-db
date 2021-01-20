// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';

import { isValidObject } from '../../../../utils/ObjectUtils';

export default class YBCheckBox extends Component {
  render() {
    const { input, label, checkState, onClick } = this.props;
    const onCheckClick = (event) => {
      if (input && input.onChange) {
        input.onChange(event);
      }
      if (isValidObject(onClick)) {
        onClick(event);
      }
    };
    return (
      <label htmlFor={this.props.name}>
        <span className="yb-input-checkbox">
          <input
            className="yb-input-checkbox__input"
            {...input}
            type="checkbox"
            name={this.props.name}
            defaultChecked={checkState}
            id={this.props.id}
            onClick={onCheckClick}
          />
          <span className="yb-input-checkbox__inner"></span>
        </span>
        <span style={{marginLeft: '6px', verticalAlign: 'middle'}}>
          {label}
        </span>
      </label>
    );
  }
}
