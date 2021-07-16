// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { Field, Formik } from 'formik';
import * as Yup from 'yup';
import 'react-bootstrap-multiselect/css/bootstrap-multiselect.css';
import { YBModal, YBFormSelect, YBFormInput } from '../../common/forms/fields';

const MIN_PASSWORD_LENGTH = 8;
export const userRoles = [
  { value: 'Admin', label: 'Admin' },
  { value: 'BackupAdmin', label: 'BackupAdmin' },
  { value: 'ReadOnly', label: 'ReadOnly' }
];

export class AddUserModal extends Component {
  submitForm = async (values) => {
    values.role = values.role.value;
    try {
      await this.props.createUser(values);
    } catch (error) {
      console.error('Failed to create user', error);
    } finally {
      this.props.onHide();
      this.props.getCustomerUsers();
    }
  };

  render() {
    const { modalVisible, onHide, passwordValidationInfo } = this.props;
    const minPasswordLength = passwordValidationInfo?.minLength || MIN_PASSWORD_LENGTH;
    const initialValues = {
      email: '',
      password: '',
      confirmPassword: '',
      role: undefined
    };

    const validationSchema = Yup.object().shape({
      email: Yup.string().required('Email is required').email('Enter a valid email'),
      password: Yup.string()
        .required('Password is required')
        .min(8, `Password is too short - must be ${minPasswordLength} characters minimum.`)
        .matches(/^(?=.*[0-9])(?=.*[!@#$%^&*])(?=.*[a-z])(?=.*[A-Z])[a-zA-Z0-9!@#$%^&*]{8,256}$/,
          `Password must contain at least ${passwordValidationInfo?.minDigits} digit, 
          ${passwordValidationInfo?.minUppercase} capital, 
          ${passwordValidationInfo?.minLowercase} lowercase 
          and ${passwordValidationInfo?.minSpecialCharacters} of the !@#$%^&* (special) characters.`),
      confirmPassword: Yup.string().oneOf([Yup.ref('password')], 'Passwords must match'),
      role: Yup.object().required('Role is required')
    });

    return (
      <Formik
        initialValues={initialValues}
        validationSchema={validationSchema}
        onSubmit={this.submitForm}
      >
        {({ handleSubmit }) => (
          <YBModal
            visible={modalVisible}
            formName="CreateUserForm"
            onHide={onHide}
            onFormSubmit={handleSubmit}
            title="Add User"
            submitLabel="Submit"
            cancelLabel="Close"
            showCancelButton
          >
            <div className="add-user-container">
              <Row className="config-provider-row">
                <Col lg={3}>
                  <div className="form-item-custom-label">Email</div>
                </Col>
                <Col lg={7}>
                  <Field name="email" placeholder="Email address" component={YBFormInput} />
                </Col>
              </Row>
              <Row className="config-provider-row">
                <Col lg={3}>
                  <div className="form-item-custom-label">Password</div>
                </Col>
                <Col lg={7}>
                  <Field
                    name="password"
                    placeholder="Password"
                    type="password"
                    component={YBFormInput}
                  />
                </Col>
              </Row>
              <Row className="config-provider-row">
                <Col lg={3}>
                  <div className="form-item-custom-label">Confirm Password</div>
                </Col>
                <Col lg={7}>
                  <Field
                    name="confirmPassword"
                    placeholder="Confirm Password"
                    type="password"
                    component={YBFormInput}
                  />
                </Col>
              </Row>
              <Row className="config-provider-row">
                <Col lg={3}>
                  <div className="form-item-custom-label">Role</div>
                </Col>
                <Col lg={7}>
                  <Field
                    name="role"
                    component={YBFormSelect}
                    options={userRoles}
                    isSearchable={false}
                  />
                </Col>
              </Row>
            </div>
          </YBModal>
        )}
      </Formik>
    );
  }
}
