import React, { Component, Fragment } from 'react';
import { YBPanelItem } from '../panels';
import { Row, Col } from 'react-bootstrap';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { Field } from 'formik';
import * as Yup from 'yup';
import { YBButton } from '../common/forms/fields';
import { YBModalForm } from '../common/forms';
import { getPromiseState } from '../../utils/PromiseUtils';
import moment from 'moment';
import { isNotHidden, isDisabled } from '../../utils/LayoutUtils';

import './certificates.scss';
import { AddCertificateFormContainer } from './';
import { YBFormInput } from '../common/forms/fields';

const validationSchema = Yup.object().shape({
  username: Yup.string()
    .required('Enter username for certificate')
});

const initialValues = {
  username: 'postgres'
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
  }

  render () {
    const { visible, onHide, certificate } = this.props;
    return (
      <YBModalForm
        validationSchema={validationSchema}
        initialValues={initialValues}
        onFormSubmit={this.handleDownload}
        formName={'downloadCertificate'}
        title={`Download Certificate ${certificate.name}`}
        id="download-cert-modal"
        visible={visible}
        onHide={onHide}
        submitLabel={"Download"}
      >
        <div className="info-text">
          Clicking download with generate <code>.crt</code> & <code>.key</code> files
        </div>
        <Row>
          <Col lg={5}>
            <Field name="username"
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
  }
  getDateColumn = (key) => (item, row) => {
    if (key in row) {
      return moment.utc(row[key], "YYYY-MM-DD hh:mm:ss a").local().calendar();
    } else {
      return null;
    }
  }

  downloadCertificates = (values) => {
    const { fetchClientCert } = this.props;
    this.setState({ showSubmitting: true });
    fetchClientCert(values.uuid, values).then((data) => {
      this.setState({ showSubmitting: false });
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
    }).catch(() => this.setState({ showSubmitting: false }));
  }

  getDownloadButton = (item, row) => {
    const payload = {
      name: row.name,
      uuid: row.uuid,
      certStart: moment(row.creationTime).valueOf(),
      certExpiry: moment(row.expiryDate).valueOf(),
    };
    return (
      <a onClick={() => {
        this.setState({selectedCert: payload});
        this.props.showDownloadCertificateModal();
      }}
        className="btn-orange"
        style={{padding: '8px 15px', borderRadius: '7px', background: '#7a6f6f', cursor: 'pointer'}}
      >
        <i className="fa fa-download"></i> Download
      </a>
    );
  }

  render() {
    const {
      customer:
        {
          currentCustomer,
          userCertificates
        },
      modal:
        {
          showModal,
          visibleModal
        },
      showAddCertificateModal
      } = this.props;
    const { showSubmitting } = this.state;

    const certificateArray = getPromiseState(userCertificates).isSuccess() ?
      userCertificates.data.map(cert => {
        return {
          uuid: cert.uuid,
          name: cert.label,
          expiryDate: cert.expiryDate,
          certificate: cert.certificate,
          creationTime: cert.startDate,
          privateKey: cert.privateKey
        };
      }) : [];

    return (
      <div id="page-wrapper">
        <YBPanelItem
          header={
            <Row className="header-row">
              <Col xs={6}>
                <h2 className="content-title">Certificates</h2>

              </Col>
              <Col xs={6} className="universe-table-header-action">
                {isNotHidden(currentCustomer.data.features, "universe.create") &&
                  <YBButton btnClass="universe-button btn btn-lg btn-orange" onClick={showAddCertificateModal}
                    disabled={isDisabled(currentCustomer.data.features, "universe.create")}
                    btnText="Add Certificate" btnIcon="fa fa-plus"/>}
              </Col>
            </Row>
          }
          body={
            <Fragment>
              <BootstrapTable data={certificateArray} className="bs-table-certs" trClassName="tr-cert-name">
                <TableHeaderColumn dataField='name' isKey={true}>Name</TableHeaderColumn>
                <TableHeaderColumn dataField='creationTime' dataFormat={this.getDateColumn('creationTime')}>Creation Time</TableHeaderColumn>
                <TableHeaderColumn dataField='expiryDate' dataFormat={this.getDateColumn('expiryDate')}>Expiration</TableHeaderColumn>
                <TableHeaderColumn dataField='certificate'>Certificate</TableHeaderColumn>
                <TableHeaderColumn dataField='privateKey'>Private Key</TableHeaderColumn>
                <TableHeaderColumn dataField='actions' dataFormat={this.getDownloadButton}>Actions</TableHeaderColumn>
              </BootstrapTable>
              <AddCertificateFormContainer
                visible={showModal && visibleModal === 'addCertificateModal'}
                onHide={this.props.closeModal}
                fetchCustomerCertificates={this.props.fetchCustomerCertificates}
                />
              <DownloadCertificateForm handleSubmit={this.downloadCertificates}
                visible={showModal && visibleModal === 'downloadCertificateModal'}
                onHide={this.props.closeModal}
                certificate={this.state.selectedCert}
              />
            </Fragment>
          }
        />
        {showSubmitting &&
          <div className="loading-text">
            <span>Generating certificates...</span><i onClick={() => this.setState({ showSubmitting: false })} className="fa fa-times"></i>
          </div>
        }
      </div>
    );    
  }
}

export default Certificates;
