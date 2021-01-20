// Copyright (c) YugaByte, Inc.

import React, { PureComponent } from 'react';
import PropTypes from 'prop-types';

import './stylesheets/DescriptionItem.scss';

export default class DescriptionItem extends PureComponent {
  static propTypes = {
    children: PropTypes.element.isRequired
  };

  render() {
    const { title } = this.props;
    return (
      <div className="description-item clearfix">
        <div className="description-item-text">{this.props.children}</div>
        <small className="description-item-sub-text">{title}</small>
      </div>
    );
  }
}
