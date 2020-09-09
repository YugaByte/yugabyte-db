import React from 'react';
import { Col, Grid, Row } from 'react-bootstrap';
import { I18n } from '../../../uikit/I18n/I18n';
import './Summary.scss';

export const Summary = () => {
  return (
    <div className="wizard-summary">
      <div className="wizard-summary__title"><I18n>Summary</I18n></div>
      <div className="wizard-summary__content">
        <div className="wizard-summary__universe"><I18n>Your Universe</I18n></div>
        <div className="wizard-summary__provider"><I18n>Provider</I18n></div>
        <Grid fluid>
          <Row className="wizard-summary__row-small">
            <Col xs={6} className="wizard-summary__col-key">
              <I18n>Nodes</I18n>
            </Col>
            <Col xs={6}  className="wizard-summary__col-value">
              0
            </Col>
          </Row>
          <Row className="wizard-summary__row-small">
            <Col xs={6} className="wizard-summary__col-key">
              <I18n>Replication Factor</I18n>
            </Col>
            <Col xs={6}  className="wizard-summary__col-value">
              0
            </Col>
          </Row>
          <Row className="wizard-summary__row-small">
            <Col xs={6} className="wizard-summary__col-key">
              <I18n>Instance Type</I18n>
            </Col>
            <Col xs={6}  className="wizard-summary__col-value">
              <I18n>Not Setup</I18n>
            </Col>
          </Row>
          <Row className="wizard-summary__row-small">
            <Col xs={6} className="wizard-summary__col-key">
              <I18n>DB Version</I18n>
            </Col>
            <Col xs={6}  className="wizard-summary__col-value">
              <I18n>Not Setup</I18n>
            </Col>
          </Row>
          <Row className="wizard-summary__row-small">
            <Col xs={6} className="wizard-summary__col-key">
              <I18n>Access Key</I18n>
            </Col>
            <Col xs={6}  className="wizard-summary__col-value">
              <I18n>Not Setup</I18n>
            </Col>
          </Row>
          <Row className="wizard-summary__row-small">
            <Col xs={6} className="wizard-summary__col-key">
              <I18n>Security</I18n>
            </Col>
            <Col xs={6}  className="wizard-summary__col-value">
              <I18n>Not Setup</I18n>
            </Col>
          </Row>
          <Row className="wizard-summary__row-small">
            <Col xs={6} className="wizard-summary__col-key">
              <I18n>Flags</I18n>
            </Col>
            <Col xs={6}  className="wizard-summary__col-value">
              <I18n>Not Setup</I18n>
            </Col>
          </Row>
          <Row className="wizard-summary__row-small">
            <Col xs={6} className="wizard-summary__col-key">
              <I18n>Read Replica</I18n>
            </Col>
            <Col xs={6}  className="wizard-summary__col-value">
              <I18n>Not Setup</I18n>
            </Col>
          </Row>
        </Grid>
      </div>
    </div>
  );
};
