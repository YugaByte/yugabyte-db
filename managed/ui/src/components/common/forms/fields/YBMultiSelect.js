// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { isFunction } from 'lodash';
import Select from 'react-select';

import { YBLabel } from '../../../../components/common/descriptors';
import { isNonEmptyArray } from '../../../../utils/ObjectUtils';

// TODO: Rename to YBMultiSelect after changing prior YBMultiSelect references.
// TODO: Make default export after checking all corresponding imports.
export class YBNewMultiSelect extends Component {
  componentDidUpdate(prevProps) {
    let newSelection = null;

    // If AZ is changed from multi to single, take only last selection.
    if (this.props.isMulti !== prevProps.isMulti && this.props.isMulti === false) {
      const currentSelection = prevProps.input.value;
      if (isNonEmptyArray(currentSelection)) {
        newSelection = currentSelection.splice(-1, 1);
      }
    }
    // If provider has been changed, reset region selection.
    if (this.props.providerSelected !== prevProps.providerSelected) {
      newSelection = [];
    }

    if (isNonEmptyArray(newSelection) && isFunction(this.props.input.onChange)) {
      this.props.input.onChange(newSelection);
    }
  }

  render() {
    const { input, options, isMulti, isReadOnly, selectValChanged, ...otherProps } = this.props;
    const self = this;
    function onChange(val) {
      val = isMulti ? val : val.slice(-1);
      if (isFunction(self.props.input.onChange)) {
        self.props.input.onChange(val);
      }
      if (selectValChanged) {
        selectValChanged(val);
      }
    }

    const customStyles = {
      option: (provided, state) => ({
        ...provided,
        padding: 10
      }),
      control: (provided) => ({
        // none of react-select's styles are passed to <Control />

        ...provided,
        width: 'auto',
        borderColor: '#dedee0',
        borderRadius: 7,
        boxShadow: 'inset 0 1px 1px rgba(0, 0, 0, .075)',
        fontSize: '14px',
        height: 42
      }),
      placeholder: (provided) => ({
        ...provided,
        color: '#999999'
      }),
      container: (provided) => ({
        ...provided,
        zIndex: 2
      }),
      dropdownIndicator: (provided) => ({
        ...provided,
        cursor: 'pointer'
      }),
      clearIndicator: (provided) => ({
        ...provided,
        cursor: 'pointer'
      }),
      singleValue: (provided, state) => {
        const opacity = state.isDisabled ? 0.5 : 1;
        const transition = 'opacity 300ms';

        return { ...provided, opacity, transition };
      }
    };

    return (
      <Select
        className="Select"
        styles={customStyles}
        {...input}
        options={options}
        disabled={isReadOnly}
        isMulti={true}
        onBlur={() => {}}
        onChange={onChange}
        {...otherProps}
      />
    );
  }
}

export default class YBMultiSelectWithLabel extends Component {
  render() {
    const { label, meta, ...otherProps } = this.props;
    return (
      <YBLabel label={label} meta={meta}>
        <YBNewMultiSelect {...otherProps} />
      </YBLabel>
    );
  }
}

// TODO: Rename all prior YBMultiSelect references to YBMultiSelectWithLabel.
export const YBMultiSelect = YBMultiSelectWithLabel;
