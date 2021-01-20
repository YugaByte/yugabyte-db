// Copyright YugaByte Inc.

import { connect } from 'react-redux';
import Certificates from './Certificates';
import { openDialog, closeDialog } from '../../../../actions/modal';
import { retrieveClientCertificate, fetchRootCertificate } from '../../../../actions/customers';

import { getTlsCertificates, getTlsCertificatesResponse } from '../../../../actions/customers';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchCustomerCertificates: () => {
      dispatch(getTlsCertificates()).then((response) => {
        dispatch(getTlsCertificatesResponse(response.payload));
      });
    },
    showAddCertificateModal: () => {
      dispatch(openDialog('addCertificateModal'));
    },
    showDownloadCertificateModal: () => {
      dispatch(openDialog('downloadCertificateModal'));
    },
    showCertificateDetailsModal: () => {
      dispatch(openDialog('certificateDetailsModal'));
    },
    closeModal: () => {
      dispatch(closeDialog());
    },
    fetchClientCert: (id, values) => {
      return dispatch(retrieveClientCertificate(id, values)).then((response) => {
        if (response.error) {
          console.error(response.payload.response);
          throw new Error('Error fetching client certificate.');
        } else {
          // Don't save the certificate in Redux store, just return directly
          return response.payload.data;
        }
      });
    },
    fetchRootCert: (id) => {
      return dispatch(fetchRootCertificate(id)).then((response) => {
        if (response.error) {
          console.error(response.payload.response);
          throw new Error('Error fetching client certificate.');
        } else {
          // Don't save the certificate in Redux store, just return directly
          return response.payload.data;
        }
      });
    }
  };
};

function mapStateToProps(state) {
  return {
    refreshReleases: state.customer.refreshReleases,
    customer: state.customer,
    modal: state.modal
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(Certificates);
