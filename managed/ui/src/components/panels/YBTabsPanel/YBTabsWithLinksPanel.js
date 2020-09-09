// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { withRouter } from 'react-router';
import { Nav, NavItem, Tab } from 'react-bootstrap';
import { isDefinedNotNull } from '../../../utils/ObjectUtils';

class YBTabsWithLinksPanel extends Component {
  static propTypes = {
    id: PropTypes.string.isRequired,
    activeTab: PropTypes.string,
    defaultTab: PropTypes.string.isRequired,
    children: PropTypes.oneOfType([PropTypes.object, PropTypes.array]),
    className: PropTypes.string,
    routePrefix: PropTypes.string,
  }

  tabSelect = selectedKey => {
    const currentLocation = this.props.location;
    if (this.props.routePrefix) {
      currentLocation.pathname = this.props.routePrefix + selectedKey;
    } else {
      currentLocation.query = currentLocation.query || {};
      currentLocation.query.tab = selectedKey;
    }
    this.props.router.push(currentLocation);
  };

  queryTabHandler = () => {
    const { location, children } = this.props;
    const locationTabKey = location.query.tab;
    if (isDefinedNotNull(locationTabKey)) {
      return children.some((item) => {
        return item.props.eventKey.indexOf(locationTabKey) >= 0 && !item.props.disabled;
      }) ? locationTabKey : false;
    }
    return false;
  }

  render() {
    const { activeTab, defaultTab } = this.props;
    const activeTabKey = activeTab || this.queryTabHandler() || defaultTab;
    const links = this.props.children.map((item) => (
      <NavItem
        key={item.props.eventKey}
        eventKey={item.props.eventKey}
        href={item.props.eventKey}
        onClick={(e) => {e.preventDefault(); this.tabSelect(item.props.eventKey);}}
      >
        {item.props.tabtitle || item.props.title}
      </NavItem>
    ));

    return (
      <Tab.Container
        defaultActiveKey={defaultTab}
        activeKey={activeTabKey}
        id={this.props.id}
        onSelect={this.tabSelect}
        className={this.props.className}
      >
        <div>
          <Nav bsStyle="tabs" className="nav nav-tabs">
            {links}
          </Nav>
          <Tab.Content animation>
            {this.props.children}
          </Tab.Content>
        </div>
      </Tab.Container>
    );
  }
}

export default withRouter(YBTabsWithLinksPanel);
