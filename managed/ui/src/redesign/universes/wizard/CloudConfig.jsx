import React from 'react';
import { Col, Grid, Row } from 'react-bootstrap';
import { I18n } from '../../uikit/I18n/I18n';
import { Input } from '../../uikit/Input/Input';
import { Select } from '../../uikit/Select/Select';
import { Toggle } from '../../uikit/Toggle/Toggle';
import { PlusButton } from '../../uikit/PlusButton/PlusButton';
import { ReplicationFactor } from './compounds/ReplicationFactor';
import './CloudConfig.scss';

export const CloudConfig = () => {
  return (
    <div className="cloud-config">
      <Grid fluid>

        <Row>
          <Col xs={12}>
            <div className="cloud-config__title">
              <I18n>Cloud Configuration</I18n>
              <div className="cloud-config__line" />
            </div>
          </Col>
        </Row>

        <Row className="cloud-config__row">
          <Col sm={2}>
            <I18n className="cloud-config__label">Name</I18n>
          </Col>
          <Col sm={8}>
            <Input />
          </Col>
        </Row>

        <Row className="cloud-config__row">
          <Col sm={2}>
            <I18n className="cloud-config__label">Provider</I18n>
          </Col>
          <Col sm={8}>
            <Select
              isSearchable={false}
              isClearable
              options={[
                { value: 1, label: 'AWS'},
                { value: 2, label: 'GCP'},
                { value: 3, label: 'Azure'},
                { value: 4, label: 'Pivotal'}
              ]}
            />
          </Col>
        </Row>

        <Row className="cloud-config__row">
          <Col sm={2}>
            <I18n className="cloud-config__label">Regions</I18n>
          </Col>
          <Col sm={10}>
            <Select
              isSearchable
              isClearable
              isMulti
              options={[
                { value: 1, label: 'US-1'},
                { value: 2, label: 'US-2'},
                { value: 3, label: 'US-3'},
                { value: 4, label: 'EU-1'},
                { value: 5, label: 'EU-2'}
              ]}
            />
          </Col>
        </Row>

        <Row className="cloud-config__row">
          <Col sm={2}>
            <I18n className="cloud-config__label">Total Nodes</I18n>
          </Col>
          <Col sm={10}>
            <div className="cloud-config__replication-factor">
              <Input type="number" className="cloud-config__number-input" />
              <I18n className="cloud-config__label">Replication Factor</I18n>
              <ReplicationFactor values={[1,3,5,7]} value={3} />
            </div>
          </Col>
        </Row>

        <Row className="cloud-config__row">
          <Col xs={12}>
            <div className="cloud-config__title">
              <I18n>Availability Zones</I18n>
              <div className="cloud-config__reset">
                <I18n>Reset Configuration</I18n>
              </div>
            </div>
          </Col>
        </Row>

        <Row className="cloud-config__row">
          <Col xs={12}>
            <div className="cloud-config__replica-placement">
              <I18n>Replica Placement</I18n>
            </div>

            <Toggle
              customTexts={{
                on: 'Manual',
                off: 'Auto'
              }}
              value={true}
              onChange={(val) => console.log(val)}
            />

          </Col>
        </Row>

        <Row className="cloud-config__row">
          <Col sm={8}>
            <I18n className="cloud-config__label">Name</I18n>
          </Col>
          <Col sm={4}>
            <I18n className="cloud-config__label">Individual Nodes</I18n>
          </Col>
        </Row>

        {[1,2,3].map(val => (
          <Row key={val} className="cloud-config__row">
            <Col sm={8}>
              <Select
                isSearchable={false}
                isClearable
                options={[
                  { value: 1, label: 'aaa'},
                  { value: 2, label: 'bbb'},
                  { value: 3, label: 'ccc'},
                  { value: 4, label: 'ddd'}
                ]}
              />
            </Col>
            <Col sm={4}>
              <Input type="number" className="cloud-config__number-input" />
            </Col>
          </Row>
        ))}
        <Row>
          <Col xs={12}>
            <br />
            <PlusButton text={<I18n>Add Row</I18n>} />
          </Col>
        </Row>


      </Grid>
    </div>
  );
};
