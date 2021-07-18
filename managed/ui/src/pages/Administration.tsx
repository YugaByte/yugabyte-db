import React, { FC, useEffect } from 'react';
import { Tab } from 'react-bootstrap';
import { Selector, useSelector } from 'react-redux';
import { RouteComponentProps } from 'react-router-dom';
import { YBTabsPanel, YBTabsWithLinksPanel } from '../components/panels';
import { showOrRedirect } from '../utils/LayoutUtils';
import { HAInstances, HAReplication } from '../components/ha';
import './Administration.scss';
import { AlertConfigurationContainer } from '../components/alerts';

// very basic redux store definition, just enough to compile without ts errors
interface Store {
  customer: {
    currentCustomer: Customer;
  };
}

interface Customer {
  data: {
    features?: Record<string, any>;
  };
}

// string values will be used in URL
enum AdministrationTabs {
  HA = 'ha',
  AC = 'alertConfig'
}
enum HighAvailabilityTabs {
  Replication = 'replication',
  Instances = 'instances'
}

enum AlertConfigurationTabs {
  Creation = 'alertCreation'
}

interface RouteParams {
  tab: AdministrationTabs;
  section: HighAvailabilityTabs | AlertConfigurationTabs;
}

// const DEFAULT_ADMIN_PAGE = `/admin/${AdministrationTabs.HA}/${HighAvailabilityTabs.Replication}`;
const customerSelector: Selector<Store, Customer> = (state) => state.customer.currentCustomer;

export const Administration: FC<RouteComponentProps<{}, RouteParams>> = ({ params }) => {
  const currentCustomer = useSelector(customerSelector);

  useEffect(() => {
    showOrRedirect(currentCustomer.data.features, 'menu.administration');
  }, [currentCustomer, params.tab, params.section]);

  return (
    <div>
      <h2 className="content-title">Platform Configuration</h2>
      <YBTabsWithLinksPanel
        defaultTab={AdministrationTabs.HA}
        activeTab={params.tab}
        routePrefix="/admin/"
        id="administration-main-tabs"
        className="universe-detail data-center-config-tab"
      >
        <Tab eventKey="ha" title="High Availability" key="high-availability">
          <YBTabsPanel
            defaultTab={HighAvailabilityTabs.Replication}
            activeTab={params.section}
            routePrefix={`/admin/${AdministrationTabs.HA}/`}
            id="administration-ha-subtab"
            className="config-tabs"
          >
            <Tab
              eventKey={HighAvailabilityTabs.Replication}
              title={
                <span>
                  <i className="fa fa-clone tab-logo" aria-hidden="true"></i> Replication
                  Configuration
                </span>
              }
              unmountOnExit
            >
              <HAReplication />
            </Tab>
            <Tab
              eventKey={HighAvailabilityTabs.Instances}
              title={
                <span>
                  <i className="fa fa-codepen tab-logo" aria-hidden="true"></i> Instance
                  Configuration
                </span>
              }
              unmountOnExit
            >
              <HAInstances />
            </Tab>
          </YBTabsPanel>
        </Tab>

        <Tab eventKey="alertConfig" title="Alert Configurations" key="alert-configurations">
          <AlertConfigurationContainer
            defaultTab={AlertConfigurationTabs.Creation}
            activeTab={params.section}
            routePrefix={`/admin/${AdministrationTabs.AC}/`}
          />
        </Tab>
      </YBTabsWithLinksPanel>
    </div>
  );
};
