// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Tab } from 'react-bootstrap';
import { browserHistory } from 'react-router';
import { isNonAvailable, isDisabled, showOrRedirect, isNotHidden } from '../../utils/LayoutUtils';
import { YBTabsWithLinksPanel } from '../panels';
import { isDefinedNotNull } from '../../utils/ObjectUtils';
import UserProfileForm from './UserProfileForm';
import AlertProfileForm from './AlertProfileForm';
import UserList from './UserList';
import { YBLoading } from '../common/indicators';
import { getPromiseState } from '../../utils/PromiseUtils';

export default class CustomerProfile extends Component {
  constructor(props) {
    super(props);
    this.state = {
      statusUpdated: false,
      updateStatus: ''
    };
  }

  componentDidMount() {
    const { customer } = this.props;
    this.props.getCustomerUsers();
    if (isNonAvailable(customer.features, 'main.profile')) browserHistory.push('/');
  }

  handleProfileUpdate = (status) => {
    this.setState({
      statusUpdated: true,
      updateStatus: status
    });
  };

  render() {
    const { customer = {}, apiToken, customerProfile, params } = this.props;
    if (getPromiseState(customer).isLoading() || getPromiseState(customer).isInit()) {
      return <YBLoading />;
    }
    showOrRedirect(customer.data.features, 'main.profile');

    let profileUpdateStatus = <span />;
    if (this.state.statusUpdated) {
      if (this.state.updateStatus === 'updated-success') {
        profileUpdateStatus = (
          <span className="pull-right request-status yb-success-color yb-dissapear">
            Profile Updated Successfully
          </span>
        );
      } else {
        profileUpdateStatus = (
          <span className="pull-right request-status yb-fail-color yb-dissapear">
            Profile Update Failed
          </span>
        );
      }
      setTimeout(() => {
        this.setState({ statusUpdated: false });
      }, 2000);
    }

    const defaultTab = isNotHidden(customer.data.features, 'main.profile') ? 'general' : 'general';
    const activeTab = isDefinedNotNull(params) ? params.tab : defaultTab;
    return (
      <div className="bottom-bar-padding">
        <h2 className="content-title">Update Customer Profile {profileUpdateStatus}</h2>
        <YBTabsWithLinksPanel
          defaultTab={defaultTab}
          activeTab={activeTab}
          routePrefix={`/profile/`}
          id={'profile-tab-panel'}
          className="profile-detail"
        >
          {[
            <Tab.Pane
              eventKey={'general'}
              tabtitle="General"
              key="general-tab"
              mountOnEnter={true}
              unmountOnExit={true}
              disabled={isDisabled(customer.data.features, 'main.profile')}
            >
              <UserProfileForm
                customer={this.props.customer}
                customerProfile={customerProfile}
                apiToken={apiToken}
                handleProfileUpdate={this.handleProfileUpdate}
                {...this.props}
              />
            </Tab.Pane>,
            <Tab.Pane
              eventKey={'health-alerting'}
              tabtitle="Health & Alerting"
              key="health-alerting-tab"
              mountOnEnter={true}
              unmountOnExit={true}
              disabled={isDisabled(customer.data.features, 'main.profile')}
            >
              <AlertProfileForm
                customer={this.props.customer}
                customerProfile={customerProfile}
                apiToken={apiToken}
                handleProfileUpdate={this.handleProfileUpdate}
                {...this.props}
              />
            </Tab.Pane>,
            <Tab.Pane
              eventKey={'manage-users'}
              tabtitle="Users"
              key="manage-users"
              mountOnEnter={true}
              unmountOnExit={true}
              disabled={isDisabled(customer.data.features, 'main.profile')}
            >
              <UserList {...this.props} />
            </Tab.Pane>
          ]}
        </YBTabsWithLinksPanel>
      </div>
    );
  }
}
