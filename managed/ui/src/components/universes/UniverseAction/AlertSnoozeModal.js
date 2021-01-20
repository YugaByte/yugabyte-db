// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import { YBFormToggle, YBFormInput } from '../../common/forms/fields';
import { Row, Col } from 'react-bootstrap';
import { Field } from 'formik';
import { YBModalForm } from '../../common/forms';
import * as Yup from 'yup';

export default class AlertSnoozeModal extends Component {
  snoozeAlert = (values) => {
    const { onFormSubmit } = this.props;
    const payload = { disabled: true };
    if (!values.disableIndefinitely && values.disablePeriodSecs > 0) {
      payload.disablePeriodSecs = values.disablePeriodSecs;
    }
    onFormSubmit(payload);
  };

  enableAlert = () => {
    const { onFormSubmit } = this.props;
    onFormSubmit({ disabled: false });
  };

  render() {
    const { visible, onHide, alertsSnoozed, disablePeriodSecs } = this.props;
    let confirmationForm;
    if (!alertsSnoozed) {
      confirmationForm = (
        <YBModalForm
          title={`Snooze Alerts for: ${this.props.universe.name}`}
          visible={visible}
          onHide={onHide}
          showCancelButton={true}
          cancelLabel={'Cancel'}
          onFormSubmit={this.snoozeAlert}
          initialValues={{
            disabled: alertsSnoozed,
            disablePeriodSecs: Math.max(disablePeriodSecs, 600),
            disableIndefinitely: true
          }}
          validationSchema={Yup.object().shape({
            disabled: Yup.bool(),
            disablePeriodSecs: Yup.number().min(0, 'Disable period must be a positive number')
          })}
          render={(props) => {
            return (
              <Fragment>
                <Row>
                  <Col lg={7}>
                    <div className="form-item-custom-label">Disable indefinitely</div>
                  </Col>
                  <Col lg={3}>
                    <Field name="disableIndefinitely" component={YBFormToggle} />
                  </Col>
                </Row>
                {!props.values.disableIndefinitely && (
                  <Field
                    name="disablePeriodSecs"
                    component={YBFormInput}
                    type={'number'}
                    label={'Snooze alerts until (secs)'}
                    placeholder={'Snooze alerts until (in seconds)'}
                  />
                )}
              </Fragment>
            );
          }}
        />
      );
    } else {
      confirmationForm = (
        <YBModalForm
          title={`Enable Alerts for: ${this.props.universe.name}`}
          visible={visible}
          onHide={onHide}
          showCancelButton={true}
          cancelLabel={'Cancel'}
          submitLabel={'Yes'}
          className="universe-action-modal"
          onFormSubmit={this.enableAlert}
        >
          <Row>
            <Col lg={12}>Are you sure you want to perform this action?</Col>
          </Row>
        </YBModalForm>
      );
    }

    return <div className="universe-apps-modal">{confirmationForm}</div>;
  }
}
