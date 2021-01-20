// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { Field, FieldArray } from 'redux-form';
import { YBInputField, YBButton } from '../../../common/forms/fields';
import { isDefinedNotNull } from '../../../../utils/ObjectUtils';

class OnPremListMachineTypes extends Component {
  UNSAFE_componentWillMount() {
    const { fields } = this.props;
    if (fields.length === 0) {
      this.props.fields.push({});
    }
  }

  addMachineTypeRow = (e) => {
    if (this.props.isEditProvider) {
      this.props.fields.push({ isBeingEdited: true });
    } else {
      this.props.fields.push({});
    }
    e.preventDefault();
  };

  removeMachineTypeRow(idx) {
    if (!this.isFieldReadOnly(idx)) {
      this.props.fields.remove(idx);
    }
  }

  isFieldReadOnly(fieldIdx) {
    const { fields, isEditProvider } = this.props;
    return (
      isEditProvider &&
      (!isDefinedNotNull(fields.get(fieldIdx).isBeingEdited) || !fields.get(fieldIdx).isBeingEdited)
    );
  }

  render() {
    const { fields } = this.props;
    const self = this;
    const removeRowButton = function (fieldIdx) {
      if (fields.length > 1) {
        return (
          <i
            className="fa fa-minus-circle on-prem-row-delete-btn"
            onClick={self.removeMachineTypeRow.bind(self, fieldIdx)}
          />
        );
      }
      return <span />;
    };
    return (
      <div>
        {fields.map(function (fieldItem, fieldIdx) {
          const isReadOnly = self.isFieldReadOnly(fieldIdx);
          return (
            <Row key={`fieldMap${fieldIdx}`}>
              <Col lg={1}>{removeRowButton(fieldIdx)}</Col>
              <Col lg={3}>
                <Field
                  name={`${fieldItem}.code`}
                  component={YBInputField}
                  insetError={true}
                  isReadOnly={isReadOnly}
                />
              </Col>
              <Col lg={1}>
                <Field
                  name={`${fieldItem}.numCores`}
                  component={YBInputField}
                  insetError={true}
                  isReadOnly={isReadOnly}
                />
              </Col>
              <Col lg={1}>
                <Field
                  name={`${fieldItem}.memSizeGB`}
                  component={YBInputField}
                  insetError={true}
                  isReadOnly={isReadOnly}
                />
              </Col>
              <Col lg={1}>
                <Field
                  name={`${fieldItem}.volumeSizeGB`}
                  component={YBInputField}
                  insetError={true}
                  isReadOnly={isReadOnly}
                />
              </Col>
              <Col lg={4}>
                <Field
                  name={`${fieldItem}.mountPath`}
                  component={YBInputField}
                  insetError={true}
                  isReadOnly={isReadOnly}
                />
              </Col>
            </Row>
          );
        })}
        <Row>
          <Col lg={1}>
            <i
              className="fa fa-plus-circle fa-2x on-prem-row-add-btn"
              onClick={this.addMachineTypeRow}
            />
          </Col>
          <Col lg={3}>
            <a className="on-prem-add-link" onClick={this.addMachineTypeRow} href="/">
              Add Instance Type
            </a>
          </Col>
        </Row>
      </div>
    );
  }
}

export default class OnPremMachineTypes extends Component {
  submitOnPremForm = (values) => {
    this.props.submitOnPremMachineTypes(values);
  };

  componentDidMount() {
    document.getElementById('onprem-machine-type-form').scrollIntoView(false);
  }

  render() {
    const { handleSubmit, switchToJsonEntry, isEditProvider } = this.props;
    return (
      <div id="onprem-machine-type-form" className="on-prem-provider-form-container">
        <form name="onPremConfigForm" onSubmit={handleSubmit(this.props.submitOnPremMachineTypes)}>
          <div className="on-prem-form-text">
            Add one or more machine types to define your hardware configuration.
          </div>
          <div className="form-field-grid">
            <Row>
              <Col lg={3} lgOffset={1}>
                Machine Type
              </Col>
              <Col lg={1}>Num Cores</Col>
              <Col lg={1}>Mem Size GB</Col>
              <Col lg={1}>Vol Size GB</Col>
              <Col lg={4}>
                Mount Paths <span className="row-head-subscript">Comma Separated</span>
              </Col>
            </Row>
            <div className="on-prem-form-grid-container">
              <FieldArray
                name="machineTypeList"
                component={OnPremListMachineTypes}
                isEditProvider={this.props.isEditProvider}
              />
            </div>
          </div>
          <div className="form-action-button-container">
            {isEditProvider ? (
              <YBButton
                btnText={'Cancel'}
                btnClass={'btn btn-default save-btn cancel-btn'}
                onClick={this.props.cancelEdit}
              />
            ) : (
              <span />
            )}
            {switchToJsonEntry}
            <YBButton btnText={'Next'} btnType={'submit'} btnClass={'btn btn-default save-btn'} />
            <YBButton
              btnText={'Previous'}
              btnClass={'btn btn-default back-btn'}
              onClick={this.props.prevPage}
            />
          </div>
        </form>
      </div>
    );
  }
}
