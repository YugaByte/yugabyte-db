import React, { Component, Fragment } from 'react';
import { YBPanelItem } from '../../../panels';
import { Row, Col, DropdownButton, MenuItem } from 'react-bootstrap';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { Field } from 'formik';
import * as Yup from 'yup';
import { YBButton } from '../../../common/forms/fields';
import { YBModalForm } from '../../../common/forms';
import { getPromiseState } from '../../../../utils/PromiseUtils';
import moment from 'moment';
import { isNotHidden, isDisabled } from '../../../../utils/LayoutUtils';

import './certificates.scss';
import { AddCertificateFormContainer } from './';
import { CertificateDetails } from './CertificateDetails';
import { YBFormInput } from '../../../common/forms/fields';

const validationSchema = Yup.object().shape({
  username: Yup.string().required('Enter username for certificate')
});

const initialValues = {
  username: 'postgres'
};

const downloadAllFilesInObject = (data) => {
  Object.entries(data).forEach((file) => {
    const [filename, content] = file;
    setTimeout(() => {
      const element = document.createElement('a');
      element.setAttribute('href', 'data:text/plain;charset=utf-8,' + encodeURIComponent(content));
      element.setAttribute('download', filename);

      element.style.display = 'none';
      document.body.appendChild(element);
      element.click();
      document.body.removeChild(element);
    });
  });
};

class DownloadCertificateForm extends Component {
  handleDownload = (values) => {
    const { certificate, handleSubmit, onHide } = this.props;
    const data = {
      ...certificate,
      username: values.username
    };
    handleSubmit(data);
    onHide();
  };

  render() {
    const { visible, onHide, certificate } = this.props;
    return (
      <YBModalForm
        validationSchema={validationSchema}
        initialValues={initialValues}
        onFormSubmit={this.handleDownload}
        formName={'downloadCertificate'}
        title={`Download YSQL Certificate ${certificate.name}`}
        id="download-cert-modal"
        visible={visible}
        onHide={onHide}
        submitLabel={'Download'}
      >
        <div className="info-text">
          Clicking download will generate <code>.crt</code> & <code>.key</code> files
        </div>
        <Row>
          <Col lg={5}>
            <Field
              name="username"
              type="text"
              label="Username"
              component={YBFormInput}
              infoContent={'Username that will be tied to certificates'}
            />
          </Col>
        </Row>
      </YBModalForm>
    );
  }
}

class Certificates extends Component {
  state = {
    showSubmitting: false,
    selectedCert: {}
  };
  getDateColumn = (key) => (item, row) => {
    if (key in row) {
      return moment.utc(row[key], 'YYYY-MM-DD hh:mm:ss a').local().calendar();
    } else {
      return null;
    }
  };

  downloadYCQLCertificates = (values) => {
    const { fetchClientCert } = this.props;
    this.setState({ showSubmitting: true });
    fetchClientCert(values.uuid, values)
      .then((data) => {
        downloadAllFilesInObject(data);
      })
      .finally(() => this.setState({ showSubmitting: false }));
  };

  downloadRootCertificate = (values) => {
    this.setState({ showSubmitting: true });
    this.props
      .fetchRootCert(values.uuid)
      .then((data) => {
        downloadAllFilesInObject(data);
      })
      .finally(() => this.setState({ showSubmitting: false }));
  };

  formatActionButtons = (cell, row) => {
    const downloadDisabled = row.type !== 'SelfSigned';
    const payload = {
      name: row.name,
      uuid: row.uuid,
      creationTime: row.creationTime,
      expiryDate: row.expiryDate
    };
    // TODO: Replace dropdown option + modal with a side panel
    return (
      <DropdownButton className="btn btn-default" title="Actions" id="bg-nested-dropdown" pullRight>
        <MenuItem
          onClick={() => {
            if (row.customCertInfo) {
              Object.assign(payload, row.customCertInfo);
            }
            this.setState({ selectedCert: payload });
            this.props.showCertificateDetailsModal();
          }}
        >
          <i className="fa fa-info-circle"></i> Details
        </MenuItem>
        <MenuItem
          onClick={() => {
            this.setState({ selectedCert: payload });
            this.props.showDownloadCertificateModal();
          }}
          disabled={downloadDisabled}
        >
          <i className="fa fa-download"></i> Download YSQL Cert
        </MenuItem>
        <MenuItem
          onClick={() => {
            this.downloadRootCertificate(row);
          }}
          disabled={downloadDisabled}
        >
          <i className="fa fa-download"></i> Download Root CA Cert
        </MenuItem>
      </DropdownButton>
    );
  };

  render() {
    const {
      customer: { currentCustomer, userCertificates },
      modal: { showModal, visibleModal },
      showAddCertificateModal
    } = this.props;
    const { showSubmitting } = this.state;

    const certificateArray = getPromiseState(userCertificates).isSuccess()
      ? userCertificates.data.map((cert) => {
        return {
          type: cert.certType,
          uuid: cert.uuid,
          name: cert.label,
          expiryDate: cert.expiryDate,
          certificate: cert.certificate,
          creationTime: cert.startDate,
          privateKey: cert.privateKey,
          customCertInfo: cert.customCertInfo
        };
      })
      : [];

    return (
      <div id="page-wrapper">
        <YBPanelItem
          header={
            <Row className="header-row">
              <Col xs={6}>
                <h2>Certificates</h2>
              </Col>
              <Col xs={6} className="universe-table-header-action">
                {isNotHidden(currentCustomer.data.features, 'universe.create') && (
                  <YBButton
                    btnClass="universe-button btn btn-lg btn-orange"
                    onClick={showAddCertificateModal}
                    disabled={isDisabled(currentCustomer.data.features, 'universe.create')}
                    btnText="Add Certificate"
                    btnIcon="fa fa-plus"
                  />
                )}
              </Col>
            </Row>
          }
          noBackground
          body={
            <Fragment>
              <BootstrapTable
                data={certificateArray}
                className="bs-table-certs"
                trClassName="tr-cert-name"
              >
                <TableHeaderColumn dataField="name" width="300px" isKey={true}>
                  Name
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="creationTime"
                  dataAlign="left"
                  dataFormat={this.getDateColumn('creationTime')}
                  width="120px"
                >
                  Creation Time
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="expiryDate"
                  dataAlign="left"
                  dataFormat={this.getDateColumn('expiryDate')}
                  width="120px"
                >
                  Expiration
                </TableHeaderColumn>
                <TableHeaderColumn dataField="certificate" width="240px" dataAlign="left">
                  Certificate
                </TableHeaderColumn>
                <TableHeaderColumn dataField="privateKey" width="240px" headerAlign="left" dataAlign="left">
                  Private Key
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="actions"
                  width="120px"
                  columnClassName="yb-actions-cell"
                  dataFormat={this.formatActionButtons}
                >
                  Actions
                </TableHeaderColumn>
              </BootstrapTable>
              <AddCertificateFormContainer
                visible={showModal && visibleModal === 'addCertificateModal'}
                onHide={this.props.closeModal}
                fetchCustomerCertificates={this.props.fetchCustomerCertificates}
              />
              <CertificateDetails
                visible={showModal && visibleModal === 'certificateDetailsModal'}
                onHide={this.props.closeModal}
                certificate={this.state.selectedCert}
              />
              <DownloadCertificateForm
                handleSubmit={this.downloadYCQLCertificates}
                visible={showModal && visibleModal === 'downloadCertificateModal'}
                onHide={this.props.closeModal}
                certificate={this.state.selectedCert}
              />
            </Fragment>
          }
        />
        {showSubmitting && (
          <div className="loading-text">
            <span>Generating certificates...</span>
            <i onClick={() => this.setState({ showSubmitting: false })} className="fa fa-times"></i>
          </div>
        )}
      </div>
    );
  }
}

export default Certificates;
