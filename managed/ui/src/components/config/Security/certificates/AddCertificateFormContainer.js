// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { AddCertificateForm } from './';

import {
  addCertificate,
  addCertificateResponse,
  getTlsCertificates,
  getTlsCertificatesResponse,
  addCertificateReset
} from '../../../../actions/customers';
import { closeDialog } from '../../../../actions/modal';

const mapDispatchToProps = (dispatch) => {
  return {
    addCertificate: (certificate, setSubmitting) => {
      dispatch(addCertificate(certificate)).then((response) => {
        dispatch(addCertificateResponse(response.payload));
        setSubmitting(false);
        if (!response.error) {
          dispatch(closeDialog());
          // fetch new certificates list

          dispatch(getTlsCertificates()).then((response) => {
            dispatch(getTlsCertificatesResponse(response.payload));
          });
        } else {
          console.error(
            'Certificate adding has been failed: ' + JSON.stringify(response.payload.data.error)
          );
        }
      });
    },
    addCertificateReset: () => {
      dispatch(addCertificateReset());
    }
  };
};

function mapStateToProps(state, ownProps) {
  const { customer } = state;

  return {
    customer: customer
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(AddCertificateForm);
