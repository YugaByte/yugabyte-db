// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';

import {
  UniverseRegionLocationPanelContainer,
  HighlightedStatsPanelContainer,
  UniverseDisplayPanelContainer
} from '../panels';
import './stylesheets/Dashboard.scss';
import { isAvailable, showOrRedirect } from '../../utils/LayoutUtils';

export default class Dashboard extends Component {
  componentDidMount() {
    this.props.fetchUniverseList();
  }

  render() {
    const {
      customer: { currentCustomer }
    } = this.props;
    showOrRedirect(currentCustomer.data.features, 'menu.dashboard');

    return (
      <div id="page-wrapper">
        {isAvailable(currentCustomer.data.features, 'main.stats') && (
          <div className="dashboard-stats">
            <HighlightedStatsPanelContainer />
          </div>
        )}
        <UniverseDisplayPanelContainer {...this.props} />
        <Row>
          <Col lg={12}>
            <UniverseRegionLocationPanelContainer {...this.props} />
          </Col>
        </Row>
      </div>
    );
  }
}
