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
import { toast } from 'react-toastify';

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
          const errorMessage = response.payload?.response?.data?.error || response.payload.message;
          toast.error(`Certificate adding has been failed:  ${errorMessage}`);
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
  }
}

export default connect(mapStateToProps, mapDispatchToProps)(AddCertificateForm);
