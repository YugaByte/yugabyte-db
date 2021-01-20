// Copyright (c) YugaByte, Inc.

import React, { Fragment, Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { YBFormInput, YBFormSelect, YBButton } from '../common/forms/fields';
import { getPromiseState } from '../../utils/PromiseUtils';
import { isNonEmptyObject } from '../../utils/ObjectUtils';
import { Highlighter } from '../../helpers/Highlighter';
import { Link } from 'react-router';
import { Formik, Field } from 'formik';
import * as Yup from 'yup';
import './Importer.scss';
import { showOrRedirect } from '../../utils/LayoutUtils';

const stepsEnum = ['BEGIN', 'IMPORTED_MASTERS', 'IMPORTED_TSERVERS', 'FINISHED'];

const stepsHeaders = {
  IMPORTED_MASTERS: 'Import masters checks',
  IMPORTED_TSERVERS: 'Import tservers checks',
  FINISHED: 'Final checks'
};

export default class Importer extends Component {
  constructor(props) {
    super(props);
    this.state = {
      currentState: 'BEGIN',
      universeUUID: '',
      checks: {}
    };
  }

  resetForm = (isUnmount) => {
    if (isUnmount) this.props.importUniverseReset();
    this.setState(
      {
        currentState: 'BEGIN',
        universeUUID: '',
        checks: {}
      },
      this.props.importUniverseReset
    );
  };

  componentWillUnmount = () => {
    if (this.state.currentState === 'FINISHED') {
      this.resetForm(true);
    }
  };

  componentDidMount() {
    const { universeImport } = this.props;
    // repopulate form if it was not finished
    if (isNonEmptyObject(universeImport.data)) {
      this.setState({
        currentState: universeImport.data.state || this.state.currentState,
        universeUUID: universeImport.data.universeUUID,
        universeName: universeImport.data.universeName || this.state.universeName,
        masterAddresses: universeImport.data.masterAddresses || this.state.masterAddresses,
        checks: { [universeImport.data.state]: { ...universeImport.data.checks } }
      });
    }
  }

  static getDerivedStateFromProps(props, state) {
    const { universeImport } = props;

    const importChecksSource =
      getPromiseState(universeImport).isError() ||
      (universeImport.data.state !== state.currentState &&
        getPromiseState(universeImport).isSuccess())
        ? getPromiseState(universeImport).isSuccess()
          ? universeImport.data
          : universeImport.error
        : {};
    const importChecks = isNonEmptyObject(importChecksSource) && {
      [importChecksSource.state]: { ...importChecksSource.checks }
    };

    if (getPromiseState(universeImport).isSuccess()) {
      return {
        currentState: universeImport.data.state,
        universeUUID: state.universeUUID || universeImport.data.universeUUID,
        universeName: state.universeName || universeImport.data.universeName,
        masterAddresses: state.masterAddresses || universeImport.data.masterAddresses,
        checks: {
          ...state.checks,
          ...importChecks
        }
      };
    } else {
      return {
        checks: {
          ...state.checks,
          ...importChecks
        }
      };
    }
  }

  submitForm = (formValues) => {
    formValues.currentState = this.state.currentState;
    formValues.universeUUID = this.state.universeUUID;
    this.props.importUniverse(formValues);
  };

  generateCheckList = () => {
    return (
      isNonEmptyObject(this.state.checks) &&
      Object.keys(this.state.checks).map((section, index) => (
        <div key={section}>
          <h5>{stepsHeaders[section]}</h5>
          <ul>
            {Object.keys(this.state.checks[section]).map((check, idx) => (
              <li
                key={section.toLowerCase() + '_' + check}
                className={this.state.checks[section][check]}
              >
                {/* TODO make checks components */}
                <i
                  className={
                    'fa fa-' + (this.state.checks[section][check] === 'OK' ? 'check' : 'times')
                  }
                ></i>
                {((str) => str.charAt(0).toUpperCase() + str.slice(1))(
                  check.replace(/_/g, ' ').replace(/db/g, 'DB')
                )}
              </li>
            ))}
          </ul>
        </div>
      ))
    );
  };

  generateProgressBar = () => {
    const currentStep = stepsEnum.indexOf(this.state.currentState);
    const barWidth = currentStep === -1 ? '0%' : 33.33 * currentStep + '%';
    const listLabels = stepsEnum.map(function (item, idx) {
      const itemClass =
        (this.props.universeImport.error && idx === currentStep - 1) ||
        (!this.props.universeImport.error && idx === currentStep && currentStep !== 0)
          ? 'finished'
          : this.props.universeImport.error && idx === currentStep + 1
          ? 'failed'
          : '';
      return (
        <li key={idx} className={itemClass + ' ' + item.class}>
          <span>{item.replace('_', ' ')}</span>
        </li>
      );
    }, this);
    return (
      <ul className="progressbar">
        <div className={'progressbar-bar finished'} style={{ width: barWidth }}></div>
        {listLabels}
      </ul>
    );
  };

  render() {
    const {
      universeImport,
      customer: { currentCustomer }
    } = this.props;
    showOrRedirect(currentCustomer.data.features, 'universe.import');

    const currentStep = stepsEnum.indexOf(this.state.currentState);

    let status = '';
    let errorString = '';
    let submitButtonTxt = 'Import Masters';
    if (getPromiseState(universeImport).isSuccess()) {
      // LOGIC get the response from the import and render it in div
      // Master OK
      //
      if (this.state.currentState === 'IMPORTED_MASTERS') {
        status = <span className="yb-success-color">Imported YB-Masters successfully</span>;
        submitButtonTxt = 'Import TServers';
      } else if (this.state.currentState === 'IMPORTED_TSERVERS') {
        status = <span className="yb-success-color">Imported YB-TServers successfully</span>;
        submitButtonTxt = 'Finalize Import';
      } else if (this.state.currentState === 'FINISHED') {
        status = <span className="yb-success-color">Successfully imported universe</span>;
        submitButtonTxt = 'Go to universes list';
      }
    } else if (universeImport.error) {
      status = <span className="yb-fail-color">Universe import failed</span>;
      errorString = universeImport.error;
      if (this.state.currentState === 'BEGIN') {
        status = <span className="yb-fail-color">Import YB-Masters failed</span>;
        submitButtonTxt = 'Import TServers';
      } else if (this.state.currentState === 'IMPORTED_MASTERS') {
        status = <span className="yb-fail-color">Import YB-TServers failed</span>;
        submitButtonTxt = 'Finalize Import';
      } else if (this.state.currentState === 'IMPORTED_TSERVERS') {
        status = <span className="yb-fail-color">Failed to finalize universe</span>;
      }
    }

    const universeProviderList = [];
    universeProviderList.unshift({ value: 'other', label: 'Other' });

    const validationSchema = Yup.object().shape({
      universeName: Yup.string().required('Universe name is Required'),
      masterAddresses: Yup.string().required('Enter the master addresses of the universe')
    });

    const initialValues = {
      universeName: this.state.universeName || '',
      cloudProviderType: universeProviderList[0],
      masterAddresses: this.state.masterAddresses || ''
    };

    return (
      <div className="bottom-bar-padding import-universe-form">
        <Formik
          validationSchema={validationSchema}
          initialValues={initialValues}
          onSubmit={(values, { setSubmitting }) => {
            const payload = {
              ...values,
              cloudProviderType: values.cloudProviderType.value
            };
            this.submitForm(payload);
            setSubmitting(false);
          }}
        >
          {({ handleSubmit, isSubmitting }) => (
            <form name="ImportUniverse" onSubmit={handleSubmit}>
              <Row>
                <h2 className="content-title">Import Existing Universe</h2>
                <Col md={8} sm={12}>
                  {this.generateProgressBar()}
                  <div className={'form-right-aligned-labels ' + (currentStep > 0 ? 'faded' : '')}>
                    <Field
                      name="universeName"
                      readOnly={currentStep > 0}
                      type="text"
                      component={YBFormInput}
                      label="Universe Name"
                      placeholder="Universe Name"
                    />
                    <Field
                      name="cloudProviderType"
                      type="select"
                      isDisabled={currentStep > 0}
                      component={YBFormSelect}
                      label="Cloud Type"
                      options={universeProviderList}
                      isClearable={false}
                    />
                    <Field
                      name="masterAddresses"
                      readOnly={currentStep > 0}
                      type="text"
                      component={YBFormInput}
                      label="Master Addresses"
                      placeholder="Master Addresses"
                    />
                  </div>
                  {this.state.currentState === 'FINISHED' ? (
                    <Link to="/universes">
                      <YBButton
                        btnText={submitButtonTxt}
                        disabled={isSubmitting || getPromiseState(universeImport).isLoading()}
                        btnClass="btn btn-default pull-right"
                      />
                    </Link>
                  ) : (
                    <Fragment>
                      <YBButton
                        btnText={'Step ' + (currentStep + 1) + ': ' + submitButtonTxt}
                        btnType="submit"
                        disabled={isSubmitting || getPromiseState(universeImport).isLoading()}
                        btnClass="btn btn-orange pull-right"
                      />
                      <YBButton
                        btnText={'Reset form'}
                        onClick={() => this.resetForm(false)}
                        disabled={isSubmitting || getPromiseState(universeImport).isLoading()}
                        btnClass="btn btn-default pull-right"
                      />
                    </Fragment>
                  )}

                  <b className="status">{status}</b>
                  {errorString && <Highlighter type="json" text={errorString} />}
                  <div className="checks">
                    {isNonEmptyObject(this.state.checks) && this.generateCheckList()}
                  </div>
                </Col>
              </Row>
            </form>
          )}
        </Formik>
      </div>
    );
  }
}
