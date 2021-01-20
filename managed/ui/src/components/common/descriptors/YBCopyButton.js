// Copyright (c) YugaByte, Inc.

import React, { PureComponent } from 'react';
import PropTypes from 'prop-types';
import copy from 'copy-to-clipboard';

export default class YBCopyButton extends PureComponent {
  static propTypes = {
    text: PropTypes.string.isRequired,
    options: PropTypes.shape({
      debug: PropTypes.bool,
      message: PropTypes.string
    })
  };
  constructor(props) {
    super(props);
    this.state = {
      clicked: false
    };
  }

  onClick = (event) => {
    const { text, options } = this.props;

    event.preventDefault();
    copy(text, options);
    this.setState({ clicked: true });
    setTimeout(() => {
      this.setState({ clicked: false });
    }, 2500);
  };

  render() {
    const { className, children } = this.props;
    const { caption, additionalClassName } = this.state.clicked
      ? { caption: 'Copied', additionalClassName: '' }
      : { caption: 'Copy', additionalClassName: ' btn-copy-inactive' };
    return (
      <button
        {...this.props}
        className={'btn btn-small btn-copy ' + className + additionalClassName}
        onClick={this.onClick}
      >
        {children || caption}
      </button>
    );
  }
}
