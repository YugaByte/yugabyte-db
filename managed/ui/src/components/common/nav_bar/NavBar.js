// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import TopNavBar from './TopNavBar';
import SideNavBar from './SideNavBar';
import './stylesheets/NavBar.scss';

export default class NavBar extends Component {
  render() {
    return (
      <div className="yb-nav-bar">
        <TopNavBar customer={this.props.customer} logoutProfile={this.props.logoutProfile} />
        <SideNavBar customer={this.props.customer} />
      </div>
    );
  }
}
