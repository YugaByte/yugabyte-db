// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';

import slackIcon from '../../common/nav_bar/images/slack-monochrome-black.svg';
import './HelpItems.scss';

export default class HelpItems extends Component {
  render() {
    return (
      <div id="page-wrapper" className="help-links">
        <h2 className="content-title">Help</h2>

        <Row>
          <Col lg={6}>
            <h4>
              <i className="fa fa-support"></i> Talk to Community
            </h4>
          </Col>
          <Col lg={6}>
            <p>
              <a href="https://www.yugabyte.com/slack" target="_blank" rel="noopener noreferrer">
                <object data={slackIcon} type="image/svg+xml" width="16">
                  Icon
                </object>{' '}
                Slack
              </a>
            </p>
            <p>
              <a href="https://forum.yugabyte.com/" target="_blank" rel="noopener noreferrer">
                <i className="fa fa-comment"></i> Forum
              </a>
            </p>
            <p>
              <a
                href="https://stackoverflow.com/questions/tagged/yugabyte-db"
                target="_blank"
                rel="noopener noreferrer"
              >
                <i className="fa fa-stack-overflow"></i> StackOverflow
              </a>
            </p>
          </Col>
        </Row>

        <Row>
          <Col lg={6}>
            <h4>
              <i className="fa fa-globe"></i> Resources
            </h4>
          </Col>
          <Col lg={6}>
            <p>
              <a href="https://docs.yugabyte.com/" target="_blank" rel="noopener noreferrer">
                <i className="fa fa-book"></i> Documentation
              </a>
            </p>
            <p>
              <a href="https://github.com/yugabyte" target="_blank" rel="noopener noreferrer">
                <i className="fa fa-github"></i> GitHub
              </a>
            </p>
          </Col>
        </Row>
        <br />
      </div>
    );
  }
}
