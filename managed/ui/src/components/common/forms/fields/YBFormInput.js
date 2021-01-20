// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { YBLabel } from '../../../../components/common/descriptors';
import { FormControl } from 'react-bootstrap';
import { isDefinedNotNull } from '../../../../utils/ObjectUtils';

export default class YBFormInput extends Component {
  handleChange = (event) => {
    const { field, onChange } = this.props;
    field.onChange(event);
    if (isDefinedNotNull(onChange)) onChange(this.props, event);
  };

  render() {
    const { infoContent, ...rest } = this.props;
    return (
      <YBLabel {...this.props} infoContent={infoContent}>
        <FormControl {...this.props.field} {...rest} onChange={this.handleChange} />
      </YBLabel>
    );
  }
}
