// Copyright (c) 2018 AppJudo Inc.  MIT License.

import React, { Component } from 'react';
import { Collapse } from 'react-bootstrap';

import './TreeNode.scss';

export default class TreeNode extends Component {
  constructor(props, context) {
    super(props, context);
    this.state = {
      open: false || props.defaultExpanded
    };
  }

  toggleOpen = () => this.setState((prevState) => ({ open: !prevState.open }));

  render() {
    const { header, body } = this.props;

    return (
      <div className={`TreeNode ${this.state.open ? 'TreeNode-open' : 'TreeNode-closed'}`}>
        <div className="TreeNode-header" onClick={this.toggleOpen}>
          <i className={`fa fa-caret-${this.state.open ? 'down' : 'right'}`} />
          <div className="TreeNode-header-content">{header}</div>
        </div>
        <Collapse in={this.state.open}>
          <div>
            <div className="TreeNode-body">{body}</div>
          </div>
        </Collapse>
      </div>
    );
  }
}
