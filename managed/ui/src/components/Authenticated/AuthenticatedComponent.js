// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { withRouter } from 'react-router';
import { isNonEmptyArray } from '../../utils/ObjectUtils';
import { getPromiseState } from '../../utils/PromiseUtils';
import { isHidden } from '../../utils/LayoutUtils';
import PropTypes from 'prop-types';

class AuthenticatedComponent extends Component {
  constructor(props) {
    super(props);
    this.state = {
      prevPath: '',
      fetchScheduled: false
    };
  }

  getChildContext() {
    return { prevPath: this.state.prevPath };
  }

  componentDidMount() {
    this.props.fetchSoftwareVersions();
    this.props.fetchTableColumnTypes();
    this.props.getEBSListItems();
    this.props.getGCPListItems();
    this.props.getAZUListItems();
    this.props.getProviderListItems();
    this.props.getSupportedRegionList();
    this.props.getYugaWareVersion();
    this.props.fetchCustomerTasks();
    this.props.fetchCustomerCertificates();
    this.props.fetchCustomerConfigs();
    this.props.fetchInsecureLogin();
  }

  componentWillUnmount() {
    this.props.resetUniverseList();
  }

  hasPendingCustomerTasks = (taskList) => {
    return isNonEmptyArray(taskList)
      ? taskList.some(
        (task) =>
          (task.status === 'Running' || task.status === 'Initializing') &&
          Number(task.percentComplete) !== 100
      )
      : false;
  };

  componentDidUpdate(prevProps) {
    const { tasks } = this.props;
    if (prevProps.fetchMetadata !== this.props.fetchMetadata && this.props.fetchMetadata) {
      this.props.getProviderListItems();
      this.props.fetchUniverseList();
      this.props.getSupportedRegionList();
    }
    if (
      prevProps.fetchUniverseMetadata !== this.props.fetchUniverseMetadata &&
      this.props.fetchUniverseMetadata
    ) {
      this.props.fetchUniverseList();
    }
    if (prevProps.location !== this.props.location) {
      this.setState({ prevPath: prevProps.location.pathname });
    }
    // Check if there are pending customer tasks and no existing recursive fetch calls
    if (this.hasPendingCustomerTasks(tasks.customerTaskList) && !this.state.fetchScheduled) {
      this.scheduleFetch();
    }
  }

  scheduleFetch = () => {
    const self = this;

    function queryTasks() {
      const taskList = self.props.tasks.customerTaskList;

      // Check if there are still customer tasks in progress or if list is empty
      if (!self.hasPendingCustomerTasks(taskList) && isNonEmptyArray(taskList)) {
        self.setState({ fetchScheduled: false });
      } else {
        self.props.fetchCustomerTasks().then(() => {
          setTimeout(queryTasks, 6000);
        });
      }
    }
    queryTasks();
    this.setState({ fetchScheduled: true });
  };

  render() {
    const { currentCustomer } = this.props;
    const sidebarHidden =
      getPromiseState(currentCustomer).isSuccess() &&
      isHidden(currentCustomer.data.features, 'menu.sidebar');
    return (
      <div
        className={sidebarHidden ? 'full-height-container sidebar-hidden' : 'full-height-container'}
      >
        {this.props.children}
      </div>
    );
  }
}

export default withRouter(AuthenticatedComponent);

AuthenticatedComponent.childContextTypes = {
  prevPath: PropTypes.string
};
