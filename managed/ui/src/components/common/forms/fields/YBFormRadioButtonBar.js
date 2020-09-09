// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { isObject } from 'lodash';
import { isNonEmptyArray, isDefinedNotNull } from '../../../../utils/ObjectUtils';
import YBFormRadioButton from './YBFormRadioButton';
import { YBLabel } from '../../descriptors';
import { Field } from 'formik';

class YBRadioButtonGroupDefault extends Component {
  render() {
    const { field, name, segmented, options, ...props } = this.props;
    return (
      <div className={"btn-group btn-group-radio"+ (segmented ? " btn-group-segmented" : "")} data-toggle="buttons">
        {options.map((option) => {
          let value, label;
          if (isNonEmptyArray(option)) {
            [value, label] = option;
          } else if (isObject(option)) {
            value = option.value;
            label = option.label;
          } else {
            value = label = option;
          }
          const key = name + "_" + value;
          return <Field component={YBFormRadioButton} key={key} id={value} name={name} label={label} segmented={segmented} {...props} />;
        })}
      </div>
    );
  }
}

export default class YBRadioButtonGroup extends Component {
  render() {
    const { label, meta, type, ...otherProps} = this.props;
    if (isDefinedNotNull(label)) {
      return (
        <YBLabel label={label} meta={meta}>
          <YBRadioButtonGroupDefault {...otherProps} />
        </YBLabel>
      );
    } else {
      return <YBRadioButtonGroupDefault {...otherProps} />;
    }
  }
}

export class YBSegmentedButtonGroup extends Component {
  render() {
    const { label, meta, type, ...otherProps} = this.props;
    if (isDefinedNotNull(label)) {
      return (
        <YBLabel label={label} meta={meta}>
          <YBRadioButtonGroupDefault segmented={true} {...otherProps} />
        </YBLabel>
      );
    } else {
      return <YBRadioButtonGroupDefault segmented={true} {...otherProps} />;
    }
  }
}

export class YBRadioButtonLine extends Component {
  constructor(props) {
    super(props);
    this.radioList = React.createRef();
    this.state = {
      selectedOption: null,
      lineStyle: {}
    };
  }

  handleSelect = (index) => {
    const { onSelect, options } = this.state;
    this.setState({
      selectedOption: index
    });
    if (onSelect) {
      onSelect(options[index]);
    }
  }

  render() {
    const { label, meta, options, ...otherProps } = this.props;

    let lineStyle = {};
    if (this.radioList.current) {
      const children = this.radioList.current.children;
      let width = 0;
      let left = 10;
      for (let i = 0; i < children.length; i++) {
        if (i === 0 && !label) {
          left = children[i].offsetWidth / 2;
        }
        if (i === children.length - 1) {
          width += children[i].offsetWidth / 2;
        } else {
          width += children[i].offsetWidth;
        }
      }
      lineStyle = {
        left: `${left}px`,
        width: `${width}px`,
      };
    }
    return (
      <YBLabel label={label} meta={meta} classOverrides={'radio-bar'} {...otherProps}>
        <ul className="yb-form-radio" ref={this.radioList}>
          {options.map((value, index) => (
            <li key={`option-${index}`}>
              <input type="radio" id={`radio-option-${index}`} name="selector"
                checked={this.state.selectedOption === index} onChange={() => this.handleSelect(index)}
              />
              <div className="check"></div>
              <label htmlFor={`radio-options-${index}`} onClick={() => this.handleSelect(index)}>{value}</label>
            </li>
          ))}
        </ul>
        <div className={"connecting-line"} style={lineStyle}></div>
      </YBLabel>
    );
  }
}
