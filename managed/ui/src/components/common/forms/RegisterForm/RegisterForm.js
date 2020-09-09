// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { PageHeader } from 'react-bootstrap';
import { YBButton, YBFormInput, YBSegmentedButtonGroup } from '../fields';
import YBLogo from '../../YBLogo/YBLogo';
import { browserHistory } from 'react-router';
import { getPromiseState } from '../../../../utils/PromiseUtils';
import { Field, Form, Formik } from 'formik';
import * as Yup from "yup";

import './RegisterForm.scss';

class RegisterForm extends Component {
  componentDidUpdate(prevProps) {
    const { customer: { authToken }} =  this.props;
    const location = Object.assign({}, browserHistory.getCurrentLocation());
    if (getPromiseState(authToken).isSuccess() && location.pathname !== '/') {
      browserHistory.push('/');
    }
  }

  submitRegister = formValues => {
    const { registerCustomer } = this.props;
    registerCustomer(formValues);
  };

  render() {
    const { customer: { authToken }} = this.props;

    const validationSchema = Yup.object().shape({
      code: Yup.string()
        .required('Enter Environment name')
        .max(5, 'Environment name can be only 5 characters long'),

      name: Yup.string()
        .required('Enter a name'),

      email: Yup.string()
        .required('Enter email')
        .email('This is not a valid email'),

      password: Yup.string()
        .required('Enter password'),

      confirmPassword: Yup.string()
        .oneOf([Yup.ref('password'), null], "Passwords don't match")
        .required('Password confirm is required'),
    });

    const initialValues = {
      code: "dev",
      name: "",
      email: "",
      password: "",
      confirmPassword: "",
    };

    return (
      <div className="container dark-background full-height page-register flex-vertical-middle">
        <div className="col-12 col-sm-10 col-md-8 col-lg-6 dark-form">
          <PageHeader bsClass="dark-form-heading">
            <YBLogo type="full"/>
            <span>Admin Console Registration</span>
          </PageHeader>
          <Formik
            validationSchema={validationSchema}
            initialValues={initialValues}
            onSubmit={(values, { setSubmitting }) => {
              this.submitRegister(values);
              setSubmitting(false);
            }}
          >
            {({ handleSubmit, isSubmitting }) => (
              <Form className="form-register" onSubmit={handleSubmit}>
                <div className={`alert alert-danger form-error-alert ${authToken.error ? '': 'hide'}`}>
                  {<strong>{JSON.stringify(authToken.error)}</strong>}
                </div>
                <div className="form-right-aligned-labels">
                  <YBSegmentedButtonGroup name="code" label="Environment" options={["dev", "demo", "stage", "prod"]} />

                  <Field name="name" type="text" component={YBFormInput} label="Full Name"/>
                  <Field name="email" type="email" component={YBFormInput} label="Email"/>
                  <Field name="password" type="password" component={YBFormInput} label="Password"/>
                  <Field name="confirmPassword" type="password" component={YBFormInput} label="Confirm Password"/>
                </div>
                <div className="clearfix">
                  <YBButton btnType="submit" disabled={isSubmitting || getPromiseState(authToken).isLoading()}
                            btnClass="btn btn-orange pull-right" btnText="Register"/>
                </div>
              </Form>
            )}
          </Formik>
        </div>
      </div>
    );
  }
}

export default RegisterForm;
