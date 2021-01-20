// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import Toggle from 'react-toggle';
import PropTypes from 'prop-types';
import { isValidObject } from '../../../../utils/ObjectUtils';
import { YBLabel, DescriptionItem } from '../../../../components/common/descriptors';
import 'react-toggle/style.css';
import './stylesheets/YBToggle.scss';

export default class YBToggle extends Component {
  render() {
    const {
      input,
      label,
      onToggle,
      isReadOnly,
      meta,
      insetError,
      subLabel,
      infoContent,
      infoTitle,
      checkedVal,
      name
    } = this.props;
    const onChange = (event) => {
      if (!this.props.disableOnChange) {
        input.onChange(event);
        if (isValidObject(onToggle)) onToggle(event);
      }
    };

    // TODO: Investigate separating this into a Controlled and Uncontrolled component. This is
    // currently a Controlled component, but relies on Redux Form in places, thus it accesses the
    // underlying input.value.
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
            checked={!!input.value && checkedVal}
            name={name}
            className="yb-toggle"
            onChange={onChange}
            disabled={isReadOnly}
          />
        </DescriptionItem>
      </YBLabel>
    );
  }
}

YBToggle.propTypes = {
  checkedVal: PropTypes.bool,
  disableOnChange: PropTypes.bool
};

YBToggle.defaultProps = {
  checkedVal: true,
  disableOnChange: false
};
