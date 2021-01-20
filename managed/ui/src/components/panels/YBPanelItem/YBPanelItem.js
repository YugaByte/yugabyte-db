// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';

import './YBPanelItem.scss';

export default class YBPanelItem extends Component {
  render() {
    const { noBackground, className, children } = this.props;
    const bodyClassName = 'body ' + (noBackground ? 'body-transparent' : '');
    return (
      <div className={className ? 'content-panel ' + className : 'content-panel'}>
        {(this.props.header || this.props.title) && (
          <div className="header">
            {this.props.header} {this.props.title}
          </div>
        )}
        {this.props.body && (
          <div className={bodyClassName}>
            {this.props.body}
            {children}
          </div>
        )}
      </div>
    );
  }
}

YBPanelItem.defaultProps = {
  hideToolBox: false
};
