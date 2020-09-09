// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { YBSelect, YBInputField } from '../../../common/forms/fields';
import { Field } from 'redux-form';

export default class InstanceTypeForRegion extends Component {
  UNSAFE_componentWillMount() {
    const {fields} = this.props;
    if (fields.length === 0) {
      this.props.fields.push({});
    }
  }

  addRow = () => {
    this.props.fields.push({});
  };

  removeRow(instanceTypeIdx) {
    this.props.fields.remove(instanceTypeIdx);
  }

  render() {
    const self = this;
    const {fields, zoneOptions, machineTypeOptions, formType, useHostname} = this.props;
    let addressType = useHostname ? 'Hostnames' : 'IP Addresses';
    if (formType === 'modal') {
      addressType = useHostname ? 'Hosts' : 'IPs';
    }

    return (
      <div className="instance-row-container">
        <Row>
          <Col lg={2} lgOffset={1}>
            Zone
          </Col>
          <Col lg={2}>
            Instance Type
          </Col>
          <Col lg={3}>
            Instances <span className="row-head-subscript">Comma Separated {addressType}</span>
          </Col>
          <Col lg={3}>
            Instance ID <span className="row-head-subscript">Comma Separated Instance Identifiers</span>
          </Col>
        </Row>
        {
          fields.map((instanceTypeItem, instanceTypeIdx) => (
            <Row key={instanceTypeIdx}>
              <Col lg={1}>
                {
                  fields.length > 1
                    ? <i className="fa fa-minus-circle on-prem-row-delete-btn" onClick={self.removeRow.bind(self, instanceTypeIdx)} />
                    : null
                }
              </Col>
              <Col lg={2}>
                <Field name={`${instanceTypeItem}.zone`} component={YBSelect} insetError={true} options={zoneOptions} />
              </Col>
              <Col lg={2}>
                <Field name={`${instanceTypeItem}.machineType`} component={YBSelect} insetError={true} options={machineTypeOptions} />
              </Col>
              <Col lg={3}>
                <Field name={`${instanceTypeItem}.instanceTypeIPs`} component={YBInputField} insetError={true} />
              </Col>
              <Col lg={3}>
                <Field name={`${instanceTypeItem}.instanceNames`} component={YBInputField} insetError={true} />
              </Col>
            </Row>
          ))
        }
        <Row>
          <Col lg={1}>
            <i className="fa fa-plus-circle fa-2x on-prem-row-add-btn" onClick={this.addRow} />
          </Col>
          <Col lg={3}>
            <a className="on-prem-add-link" onClick={this.addRow}>Add </a>
          </Col>
        </Row>
      </div>
    );
  }
}
