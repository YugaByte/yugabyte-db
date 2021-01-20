// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Tab } from 'react-bootstrap';
import { YBTabsPanel } from '../../panels';
import KeyManagementConfigurationContainer from './KeyManagementConfigurationContainer';
import { CertificatesContainer } from './certificates';

const EncryptionAtRestTabHeader = (
  <div className="on-premise">
    <i className="fa fa-lock" />Encryption <br /> At Rest
  </div>
);

const CertificatesTabHeader = (
  <div className="on-premise">
    <i className="fa fa-chain"/>Encryption <br /> In Transit
  </div>
);

const TAB_AT_REST = 'encryption-at-rest';
const TAB_IN_TRANSIT = 'encryption-in-transit';

class SecurityConfiguration extends Component {
  render() {
    const activeTab = this.props.activeTab || TAB_AT_REST;

    return (
      <YBTabsPanel
        defaultTab={TAB_AT_REST}
        activeTab={activeTab}
        id="security-tab-panel"
        className="config-tabs"
        routePrefix="/config/security/"
      >
        <Tab eventKey={TAB_AT_REST} title={EncryptionAtRestTabHeader}>
          <KeyManagementConfigurationContainer />
        </Tab>
        <Tab eventKey={TAB_IN_TRANSIT} title={CertificatesTabHeader}>
          <CertificatesContainer />
        </Tab>
      </YBTabsPanel>
    );
  }
}
export default SecurityConfiguration;
