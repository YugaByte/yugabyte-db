// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Link, IndexLink, withRouter } from 'react-router';
import { NavDropdown } from 'react-bootstrap';
import slackIcon from './images/slack-monochrome-black.svg';
import './stylesheets/SideNavBar.scss';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { isHidden, isNotHidden, getFeatureState } from '../../../utils/LayoutUtils';

class NavLink extends Component {
  render() {
    const { router, display, index, to, icon, text, ...props } = this.props;

    // Added by withRouter in React Router 3.0.
    delete props.params;
    delete props.location;
    delete props.routes;
    const isActive = router.isActive(to, index);
    const LinkComponent = index ? IndexLink : Link;

    if (!display || display === 'hidden') return null;

    return (
      <li className={`${isActive ? 'active' : ''}${display === 'disabled' ? ' disabled' : ''}`}>
        {display === 'disabled' ? (
          <div>
            <i className={icon}></i>
            <span>{text}</span>
          </div>
        ) : (
          <LinkComponent to={to} title={text} disabled={display === 'disabled'} {...props}>
            <i className={icon}></i>
            <span>{text}</span>
          </LinkComponent>
        )}
      </li>
    );
  }
}

NavLink = withRouter(NavLink);

export default class SideNavBar extends Component {
  render() {
    const {
      customer: { currentCustomer }
    } = this.props;

    // Add check for initial state of `currentCustomer` to avoid first load showing the sidebar
    // Just in case we are on cloud and don't want to cause visual flicker
    if (
      getPromiseState(currentCustomer).isInit() ||
      isHidden(currentCustomer.data.features, 'menu.sidebar')
    ) {
      return null;
    }

    return (
      <div className="side-nav-container">
        <div className="left_col">
          <div className="left_col scroll-view">
            {(getPromiseState(currentCustomer).isSuccess() ||
              getPromiseState(currentCustomer).isInit()) && (
              <div id="sidebar-menu" className="main-menu-side hidden-print main-menu">
                <div className="menu_section">
                  <ul className="nav side-menu">
                    <NavLink
                      to="/"
                      icon="fa fa-dashboard"
                      text="Dashboard"
                      display={getFeatureState(currentCustomer.data.features, 'menu.dashboard')}
                      index={true}
                    />
                    <NavLink
                      to="/universes"
                      icon="fa fa-globe"
                      text="Universes"
                      display={getFeatureState(currentCustomer.data.features, 'menu.universes')}
                    />
                    <NavLink
                      to="/metrics"
                      icon="fa fa-line-chart"
                      text="Metrics"
                      display={getFeatureState(currentCustomer.data.features, 'menu.metrics')}
                    />
                    <NavLink
                      to="/tasks"
                      icon="fa fa-list"
                      text="Tasks"
                      display={getFeatureState(currentCustomer.data.features, 'menu.tasks')}
                    />
                    <NavLink
                      to="/alerts"
                      icon="fa fa-bell-o"
                      text="Alerts"
                      display={getFeatureState(currentCustomer.data.features, 'menu.alerts')}
                    />
                    <NavLink
                      to="/config"
                      icon="fa fa-cloud-upload"
                      text="Configs"
                      display={getFeatureState(currentCustomer.data.features, 'menu.config')}
                    />
                  </ul>
                  {isNotHidden(currentCustomer.data.features, 'menu.help') && (
                    <ul className="nav side-menu position-bottom">
                      <NavDropdown
                        dropup
                        eventKey="2"
                        title={
                          <div>
                            <i className="fa fa-question"></i>
                            <span>Help</span>
                          </div>
                        }
                        id="help-dropdown"
                      >
                        <h4>Talk to Community</h4>
                        <li>
                          <a
                            href="https://www.yugabyte.com/slack"
                            target="_blank"
                            rel="noopener noreferrer"
                          >
                            <object data={slackIcon} type="image/svg+xml" width="16">
                              Icon
                            </object>{' '}
                            Slack
                          </a>
                        </li>
                        <li>
                          <a
                            href="https://forum.yugabyte.com/"
                            target="_blank"
                            rel="noopener noreferrer"
                          >
                            <i className="fa fa-comment"></i> Forum
                          </a>
                        </li>
                        <li>
                          <a
                            href="https://stackoverflow.com/questions/tagged/yugabyte-db"
                            target="_blank"
                            rel="noopener noreferrer"
                          >
                            <i className="fa fa-stack-overflow"></i> StackOverflow
                          </a>
                        </li>
                        <h4>Resources</h4>
                        <li>
                          <a
                            href="https://docs.yugabyte.com/"
                            target="_blank"
                            rel="noopener noreferrer"
                          >
                            <i className="fa fa-book"></i> Documentation
                          </a>
                        </li>
                        <li>
                          <a
                            href="https://github.com/yugabyte"
                            target="_blank"
                            rel="noopener noreferrer"
                          >
                            <i className="fa fa-github"></i> GitHub
                          </a>
                        </li>
                      </NavDropdown>
                    </ul>
                  )}
                </div>
              </div>
            )}
          </div>
        </div>
      </div>
    );
  }
}
