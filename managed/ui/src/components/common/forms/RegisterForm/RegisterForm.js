// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { PageHeader } from 'react-bootstrap';
import { YBButton, YBCheckBox, YBFormInput, YBSegmentedButtonGroup } from '../fields';
import YBLogo from '../../YBLogo/YBLogo';
import { browserHistory } from 'react-router';
import { getPromiseState } from '../../../../utils/PromiseUtils';
import { Field, Form, Formik } from 'formik';
import * as Yup from 'yup';

import './RegisterForm.scss';

const MIN_PASSWORD_LENGTH = 8;
class RegisterForm extends Component {
  componentDidMount() {
    const { validateRegistration } = this.props;
    validateRegistration();
  }

  componentDidUpdate(prevProps) {
    const {
      customer: { authToken }
    } = this.props;
    const location = Object.assign({}, browserHistory.getCurrentLocation());
    if (getPromiseState(authToken).isSuccess() && location.pathname !== '/') {
      browserHistory.push('/');
    }
  }

  submitRegister = (formValues) => {
    const { registerCustomer } = this.props;
    registerCustomer(formValues);
  };

  render() {
    const {
      customer: { authToken }, passwordValidationInfo
    } = this.props;
    const minPasswordLength = passwordValidationInfo?.minLength || MIN_PASSWORD_LENGTH;

    const validationSchema = Yup.object().shape({
      code: Yup.string()
        .required('Enter Environment name')
        .max(5, 'Environment name can be only 5 characters long'),

      name: Yup.string().required('Enter a name'),

      email: Yup.string().required('Enter email').email('This is not a valid email'),

      password: Yup.string()
        .required('Enter password')
        .min(minPasswordLength, `Password is too short - must be ${minPasswordLength} characters minimum.`)
        .matches(/^(?=.*[0-9])(?=.*[!@#$%^&*])(?=.*[a-z])(?=.*[A-Z])[a-zA-Z0-9!@#$%^&*]{8,256}$/,
          `Password must contain at least ${passwordValidationInfo?.minDigits} digit
          , ${passwordValidationInfo?.minUppercase} capital
          , ${passwordValidationInfo?.minLowercase} lowercase
          and ${passwordValidationInfo?.minSpecialCharacters} of the !@#$%^&* (special) characters.`),

      confirmPassword: Yup.string()
        .oneOf([Yup.ref('password'), null], "Passwords don't match")
        .required('Password confirm is required'),

      confirmEULA: Yup.bool().oneOf([true], 'Please accept the agreement to continue.')
    });

    const initialValues = {
      code: 'dev',
      name: '',
      email: '',
      password: '',
      confirmPassword: '',
      confirmEULA: false
    };

    return (
      <div className="container dark-background full-height page-register flex-vertical-middle">
        <div className="col-12 col-sm-10 col-md-8 col-lg-6 dark-form">
          <PageHeader bsClass="dark-form-heading">
            <YBLogo type="full" />
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
            {({ handleSubmit, isSubmitting, isValid }) => (
              <Form className="form-register" onSubmit={handleSubmit}>
                <div
                  className={`alert alert-danger form-error-alert ${authToken.error ? '' : 'hide'}`}
                >
                  {<strong>{JSON.stringify(authToken.error)}</strong>}
                </div>
                <div className="form-right-aligned-labels">
                  <YBSegmentedButtonGroup
                    name="code"
                    label="Environment"
                    options={['dev', 'demo', 'stage', 'prod']}
                  />

                  <Field name="name" type="text" component={YBFormInput} label="Full Name" />
                  <Field name="email" type="email" component={YBFormInput} label="Email" />
                  <Field name="password" type="password" component={YBFormInput} label="Password" />
                  <Field
                    name="confirmPassword"
                    type="password"
                    component={YBFormInput}
                    label="Confirm Password"
                  />
                </div>
                <div className="clearfix form-register__footer">
                  <div className="confirm-eula">
                    <Field name="confirmEULA" component={YBCheckBox} />
                    <div>I agree to Yugabyte, Inc's <a href="https://www.yugabyte.com/eula/" target="_blank" rel="noreferrer noopener">End User License Agreement</a>.</div>
                  </div>
                  <YBButton
                    btnType="submit"
                    disabled={isSubmitting || !isValid || getPromiseState(authToken).isLoading()}
                    btnClass="btn btn-orange pull-right"
                    btnText="Register"
                  />
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
