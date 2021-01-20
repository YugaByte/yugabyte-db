// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { Field } from 'formik';
import * as Yup from 'yup';

import { YBFormInput } from '../../../components/common/forms/fields';
import { YBModalForm } from '../../../components/common/forms';

export default class ImportRelease extends Component {
  importRelease = (values) => {
    const { importYugaByteRelease, onHide, onModalSubmit } = this.props;
    importYugaByteRelease(values);
    onHide();
    onModalSubmit();
  };

  render() {
    const { visible, onHide } = this.props;

    return (
      <div className="universe-apps-modal">
        <YBModalForm
          title={'Import Release'}
          visible={visible}
          onHide={onHide}
          showCancelButton={true}
          cancelLabel={'Cancel'}
          className="import-release-modal"
          onFormSubmit={this.importRelease}
          initialValues={this.props.initialValues}
          validationSchema={Yup.object().shape({
            version: Yup.string()
              .matches(/^((\d+).(\d+).(\d+).(\d+)(?:-[a-z]+)?(\d+)?)$/, {
                message: 'Incorrect version format. Valid formats: 1.1.1.1 or 1.1.1.1-b1',
                excludeEmptyString: true
              })
              .required('Release Version is Required')
          })}
        >
          <Row>
            <Col lg={12}>
              <Field name="version" component={YBFormInput} label={'Release Version'} />
            </Col>
          </Row>
        </YBModalForm>
      </div>
    );
  }
}
