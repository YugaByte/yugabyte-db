// Copyright (c) YugaByte, Inc.

import React, { Component }  from 'react';
import 'bootstrap/dist/css/bootstrap.css';
import './stylesheets/App.scss';
import './../_style/fonts.css';

export default class App extends Component {
  render() {
    return (
      <div>
        {this.props.children}
      </div>
    );
  }
}
