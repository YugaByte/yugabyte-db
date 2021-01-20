// Copyright (c) YugaByte, Inc.

import React, { PureComponent, Fragment } from 'react';
import { Button } from 'react-bootstrap';
import { YBLoadingLinearIcon } from '../../indicators';
import { isDefinedNotNull } from '../../../../utils/ObjectUtils';

export default class YBButton extends PureComponent {
  render() {
    const {
      btnClass,
      btnText,
      btnIcon,
      btnSize,
      btnType,
      btnStyle,
      disabled,
      loading,
      ...otherProps
    } = this.props;
    const className = isDefinedNotNull(btnText) ? btnIcon : `${btnIcon} no-margin no-padding`;
    return (
      <Button
        bsClass={btnClass}
        type={btnType}
        onClick={this.props.onClick}
        bsSize={btnSize}
        bsStyle={btnStyle}
        disabled={disabled}
        {...otherProps}
      >
        {loading ? (
          <YBLoadingLinearIcon />
        ) : (
          <Fragment>
            {className && <i className={className}></i>}
            {btnText}
          </Fragment>
        )}
      </Button>
    );
  }
}
