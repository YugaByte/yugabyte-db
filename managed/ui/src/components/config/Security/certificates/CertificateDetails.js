// Copyright (c) YugaByte, Inc.

import React, { Fragment } from 'react';
import { YBModal } from '../../../common/forms/fields';

export const CertificateDetails = ({certificate, visible, onHide}) => {
  const certStart = certificate.creationTime
    ? new Date(certificate.creationTime).toLocaleDateString('default', {
        month: 'long',
        day: 'numeric',
        year: 'numeric'
      })
    : '';
  const certExpiry = certificate.expiryDate
    ? new Date(certificate.expiryDate).toLocaleDateString('default', {
        month: 'long',
        day: 'numeric',
        year: 'numeric'
      })
    : '';

  return (
    <div className="cert-details-modal">
      <YBModal
        title={'Certificate Details'}
        visible={visible}
        onHide={onHide}
        submitLabel={'Close'}
        onFormSubmit={onHide}
      >
        <ul className="cert-details-modal__list">
          <li><label>Certificate Name</label><div>{certificate.name}</div></li>
          <li><label>Certificate Start</label><div>{certStart}</div></li>
          <li><label>Certificate Expiration</label><div>{certExpiry}</div></li>
          {certificate.rootCertPath &&
            <Fragment>
              <li><label>Root CA Certificate</label><div>{certificate.rootCertPath}</div></li>
              <li><label>Database Node Certificate Path</label><div>{certificate.nodeCertPath}</div></li>
              <li><label>Database Node Certificate Private Key</label><div>{certificate.nodeKeyPath}</div></li>
              <li><label>Client Certificate</label><div>{certificate.clientCertPath || '---'}</div></li>
              <li><label>Client Certificate Private Key</label><div>{certificate.clientKeyPath || '---'}</div></li>
            </Fragment>
          }
        </ul>
      </YBModal>
    </div>
  );
};
