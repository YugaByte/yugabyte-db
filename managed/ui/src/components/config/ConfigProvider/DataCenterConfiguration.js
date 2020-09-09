// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { withRouter } from 'react-router';
import { KubernetesProviderConfigurationContainer,
         OnPremConfigurationContainer, ProviderConfigurationContainer, StorageConfigurationContainer,
         SecurityConfiguration } from '../../config';
import { Tab, Row, Col } from 'react-bootstrap';
import { YBTabsPanel } from '../../panels';
import './providerConfig.scss';
import awsLogo from './images/aws.svg';
import azureLogo from './images/azure.png';
import k8sLogo from './images/k8s.png';
import pksLogo from './images/pks.png';
import gcpLogo from './images/gcp.png';
import { isAvailable, showOrRedirect } from '../../../utils/LayoutUtils';


class DataCenterConfiguration extends Component {

  render() {
    const { customer: { currentCustomer }, params: { tab, section }, params } = this.props;
    showOrRedirect(currentCustomer.data.features, "menu.config");

    const onPremiseTabContent = (
      <div className="on-premise">
        <i className="fa fa-server" />
        On-Premises<br/>Datacenters
      </div>
    );

    const k8sTabContent = (
      <Row className="custom-tab">
        <Col md={4}>
          <img src={k8sLogo} alt="Managed Kubernetes" className="k8s-logo" />
        </Col>
        <Col md={8}>
          Managed<br/>Kubernetes Service
        </Col>
      </Row>
    );

    const pksTabContent = (
      <Row className="custom-tab">
        <Col md={4} >
          <img src={pksLogo} alt="Pivotal Container Service" className="pks-logo" />
        </Col>
        <Col md={8} className="provider-map-container">
          Pivotal<br/>Container Service
        </Col>
      </Row>
    );

    const defaultTab = isAvailable(currentCustomer.data.features, "config.infra") ? "cloud" : "backup";
    const activeTab = tab || defaultTab;

    return (
      <div>
        <h2 className="content-title">Cloud Provider Configuration</h2>
        <YBTabsPanel defaultTab={defaultTab} activeTab={activeTab} routePrefix="/config/" id="config-tab-panel">
          {isAvailable(currentCustomer.data.features, "config.infra") &&
            <Tab eventKey="cloud" title="Infrastructure" key="cloud-config">
              <YBTabsPanel defaultTab="aws" activeTab={section} id="cloud-config-tab-panel" className="config-tabs" routePrefix="/config/cloud/">
                <Tab eventKey="aws" title={<img src={awsLogo} alt="AWS" className="aws-logo" />} key="aws-tab" unmountOnExit={true}>
                  <ProviderConfigurationContainer providerType="aws" />
                </Tab>
                <Tab eventKey="gcp" title={<img src={gcpLogo} alt="GCP" className="gcp-logo" />} key="gcp-tab" unmountOnExit={true}>
                  <ProviderConfigurationContainer providerType="gcp" />
                </Tab>
                <Tab eventKey="azure" title={<img src={azureLogo} alt="Azure" className="azure-logo" />} key="azure-tab" unmountOnExit={true}>
                  <ProviderConfigurationContainer providerType="azu" />
                </Tab>
                <Tab eventKey="pks" title={pksTabContent} key="pks-tab" unmountOnExit={true}>
                  <KubernetesProviderConfigurationContainer type="pks" params={params} />
                </Tab>
                <Tab eventKey="k8s" title={k8sTabContent} key="k8s-tab" unmountOnExit={true}>
                  <KubernetesProviderConfigurationContainer type="k8s" params={params} />
                </Tab>
                <Tab eventKey="onprem" title={onPremiseTabContent} key="onprem-tab" unmountOnExit={true}>
                  <OnPremConfigurationContainer params={params} />
                </Tab>
              </YBTabsPanel>
            </Tab>
          }
          {isAvailable(currentCustomer.data.features, "config.backup") &&
            <Tab eventKey="backup" title="Backup" key="storage-config">
              <StorageConfigurationContainer activeTab={section} />
            </Tab>
          }
          {isAvailable(currentCustomer.data.features, "config.security") &&
            <Tab eventKey="security" title="Security" key="security-config">
              <SecurityConfiguration activeTab={section} />
            </Tab>
          }
        </YBTabsPanel>
      </div>
    );
  }
}
export default withRouter(DataCenterConfiguration);
