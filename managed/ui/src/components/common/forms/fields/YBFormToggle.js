// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import Toggle from 'react-toggle';
import { YBLabel, DescriptionItem } from '../../../../components/common/descriptors';
import 'react-toggle/style.css';
import './stylesheets/YBToggle.scss';

export default class YBFormToggle extends Component {
  render() {
    const {
      label,
      isReadOnly,
      meta,
      insetError,
      subLabel,
      infoContent,
      infoTitle,
      field
    } = this.props;
    const onChange = (event) => {
      if (field.onChange) {
        field.onChange(event);
      }
    };
    return (
      <YBLabel
        label={label}
        meta={meta}
        insetError={insetError}
        infoContent={infoContent}
        infoTitle={infoTitle}
      >
        <DescriptionItem title={subLabel}>
          <Toggle
            checked={field.value}
            name={field.name}
            className="yb-toggle"
            onChange={onChange}
            disabled={isReadOnly}
          />
        </DescriptionItem>
      </YBLabel>
    );
  }
}
