// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { Link } from 'react-router';
import { YBButton } from '../../../components/common/forms/fields';
import { UniverseTableContainer } from '../../../components/universes';
import { HighlightedStatsPanelContainer } from '../../panels';
import { isNotHidden, isDisabled, isAvailable, showOrRedirect } from '../../../utils/LayoutUtils';

import './ListUniverse.scss';

export default class ListUniverse extends Component {
  componentDidMount() {
    this.props.fetchUniverseList();
  }

  render() {
    const {
      customer: { currentCustomer }
    } = this.props;
    showOrRedirect(currentCustomer.data.features, 'main.universe_list');

    return (
      <div id="page-wrapper">
        <Row className="header-row">
          <Col xs={6}>
            <h2 className="content-title">Universes</h2>
            {isAvailable(currentCustomer.data.features, 'main.stats') && (
              <div className="dashboard-stats">
                <HighlightedStatsPanelContainer />
              </div>
            )}
          </Col>
          <Col xs={6} className="universe-table-header-action">
            {isNotHidden(currentCustomer.data.features, 'universe.import') && (
              <Link to="/universes/import">
                <YBButton
                  btnClass="universe-button btn btn-lg btn-default"
                  disabled={isDisabled(currentCustomer.data.features, 'universe.import')}
                  btnText="Import Universe"
                  btnIcon="fa fa-mail-forward"
                />
              </Link>
            )}
            {isNotHidden(currentCustomer.data.features, 'universe.create') && (
              <Link to="/universes/create">
                <YBButton
                  btnClass="universe-button btn btn-lg btn-orange"
                  disabled={isDisabled(currentCustomer.data.features, 'universe.create')}
                  btnText="Create Universe"
                  btnIcon="fa fa-plus"
                />
              </Link>
            )}
          </Col>
        </Row>
        <UniverseTableContainer />
      </div>
    );
  }
}
