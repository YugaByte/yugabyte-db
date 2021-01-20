// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';

import './YBWidget.scss';
import { FlexContainer, FlexGrow } from '../../common/flexbox/YBFlexBox';

export default class YBWidget extends Component {
  render() {
    const { className, noMargin, children } = this.props;
    const size = this.props.size || 1;
    return (
      <div className={'widget-panel widget-panel-row-' + size + (className ? ' ' + className : '')}>
        <FlexContainer className="header">
          {this.props.headerLeft && <FlexGrow className="left">{this.props.headerLeft}</FlexGrow>}
          {this.props.headerRight && (
            <FlexGrow className="right">{this.props.headerRight}</FlexGrow>
          )}
        </FlexContainer>
        <div className={noMargin ? 'body body-no-margin' : 'body'}>
          {this.props.body && (
            <Fragment>
              {this.props.body}
              {children}
            </Fragment>
          )}
        </div>
      </div>
    );
  }
}

YBWidget.defaultProps = {
  hideToolBox: false
};
