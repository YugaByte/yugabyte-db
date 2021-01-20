// Copyright (c) YugaByte, Inc.

import Cookies from 'js-cookie';
import React from 'react';
import { Route, IndexRoute, browserHistory } from 'react-router';
import _ from 'lodash';

import { validateToken, validateFromTokenResponse,
  fetchCustomerCount, customerTokenError,
  resetCustomer, insecureLogin,
  insecureLoginResponse } from './actions/customers';
import App from './app/App';
import Login from './pages/Login';
import Register from './pages/Register';
import AuthenticatedComponent from './pages/AuthenticatedComponent';
import Dashboard from './pages/Dashboard';
import UniverseDetail from './pages/UniverseDetail';
import Universes from './pages/Universes';
import { Tasks, TasksList, TaskDetail } from './pages/tasks';
import Alerts from './pages/Alerts';
import ListUniverse from './pages/ListUniverse';
import Metrics from './pages/Metrics';
import DataCenterConfiguration from './pages/DataCenterConfiguration';
import TableDetail from './pages/TableDetail';
import Help from './pages/Help';
import Profile from './pages/Profile';
import YugawareLogs from './pages/YugawareLogs';
import Importer from './pages/Importer';
import Releases from './pages/Releases';
import { isDefinedNotNull } from './utils/ObjectUtils';
import { CreateUniverse } from './redesign/universe/CreateUniverse';
import { EditUniverse } from './redesign/universe/EditUniverse';

export const clearCredentials = () => {
  localStorage.removeItem('authToken');
  localStorage.removeItem('apiToken');
  localStorage.removeItem('customerId');
  localStorage.removeItem('userId');
  Cookies.remove('apiToken');
  Cookies.remove('authToken');
  Cookies.remove('customerId');
  Cookies.remove('userId');
  browserHistory.push('/');
};

function validateSession(store, replacePath, callback) {
  // Attempt to route to dashboard if tokens and cUUID exists or if insecure mode is on.
  // Otherwise, go to login/register.
  const userId = Cookies.get('userId') || localStorage.getItem('userId');
  const customerId = Cookies.get('customerId') || localStorage.getItem('customerId');
  if (_.isEmpty(customerId) || _.isEmpty(userId)) {
    store.dispatch(insecureLogin()).then((response) => {
      if (response.payload.status === 200) {
        store.dispatch(insecureLoginResponse(response));
        localStorage.setItem('apiToken', response.payload.data.apiToken);
        localStorage.setItem('customerId', response.payload.data.customerUUID);
        localStorage.setItem('userId', response.payload.data.userUUID);
        // Show the intro modal if OSS version
        if (localStorage.getItem('__yb_new_user__') == null) {
          localStorage.setItem('__yb_new_user__', true);
        }
        browserHistory.push('/');
      }
    });
    store.dispatch(fetchCustomerCount()).then((response) => {
      if (!response.error) {
        const responseData = response.payload.data;
        if (responseData && responseData.count === 0) {
          browserHistory.push('/register');
        }
      }
    });
    store.dispatch(customerTokenError());
    browserHistory.push('/login');
  } else {
    store.dispatch(validateToken())
      .then((response) => {
        if (response.error) {
          const { status } = isDefinedNotNull(response.payload.response) ? response.payload.response : {};
          switch (status) {
            case 403:
              store.dispatch(resetCustomer());
              store.dispatch(customerTokenError());
              clearCredentials();
              break;
            default:
              // Do nothing
          }
          return;
        }

        store.dispatch(validateFromTokenResponse(response.payload));
        if (response.payload.status === 200) {
          // update userId and customerId in local storage on successful token validation
          if ("uuid" in response.payload.data) {
            localStorage.setItem("customerId", response.payload.data["uuid"]);
          }
          localStorage.setItem('userId', userId);
        } else {
          store.dispatch(resetCustomer());
          clearCredentials();
          callback();
        }
      });
  }
  // TODO: Customer configs are sometimes not updated because callback does not wait for tokens
  // to be validated. However, having callback wait for the response will cause an infinite loop
  // when redirecting (e.g. showOrRedirect).
  callback();
}

export default (store) => {
  const authenticatedSession = (nextState, replace, callback) => {
    validateSession(store, replace, callback);
  };

  const checkIfAuthenticated = (prevState, nextState, replace, callback) => {
    validateSession(store, replace, callback);
  };

  return (
    // We will have two different routes, on which is authenticated route
    // rest un-authenticated route
    <Route path="/" component={App}>
      <Route path="/login" component={Login} />
      <Route path="/register" component={Register} />
      <Route onEnter={authenticatedSession} onChange={checkIfAuthenticated} component={AuthenticatedComponent}>
        <IndexRoute component={Dashboard} />
        <Route path="/universes" component={Universes} >
          <IndexRoute component={ListUniverse} />
          <Route path="/universes/import" component={Importer} />
          <Route path="/universes/create" component={UniverseDetail} />
          <Route path="/universes/:uuid" component={UniverseDetail} />
          <Route path="/universes/:uuid/edit" component={UniverseDetail} >
            <Route path="/universes/:uuid/edit/:type" component={UniverseDetail} />
          </Route>
          <Route path="/universes/:uuid/:tab" component={UniverseDetail} />
          <Route path="/universes/:uuid/tables/:tableUUID" component={TableDetail}/>
        </Route>

        {/* ------------------------------------------------------------------------*/}
        <Route path="/universe/create" component={CreateUniverse} />
        {/* <Route path="/universe/:universeId/create/async" component={CreateUniverse} /> - create async cluster, not supported at the moment */}
        <Route path="/universe/:universeId/edit/primary" component={EditUniverse} />

        {/* <Route path="/universe/:universeId/edit/primary/:wizardStep" component={EditUniverse} /> - jump to particular step on editing primary cluster, not supported at the moment */}
        {/* <Route path="/universe/:universeId/edit/async" component={EditUniverse} /> - edit the only async cluster, not supported at the moment */}
        {/* <Route path="/universe/:universeId/edit/async/:asyncClusterId" component={EditUniverse} /> - edit specific async cluster, not supported at the moment */}
        {/* <Route path="/universe/:universeId/edit/async/:asyncClusterId/:wizardStep" component={EditUniverse} /> - jump to particular step for editing specific async cluster, not supported at the moment */}
        {/* ------------------------------------------------------------------------*/}

        <Route path="/tasks" component={Tasks} >
          <IndexRoute component={TasksList}/>
          <Route path="/tasks/:taskUUID" component={TaskDetail}/>
        </Route>
        <Route path="/metrics" component={Metrics} />
        <Route path="/config" component={DataCenterConfiguration}>
          <Route path="/config/:tab" component={DataCenterConfiguration} />
          <Route path="/config/:tab/:section" component={DataCenterConfiguration} />
          <Route path="/config/:tab/:section/:uuid" component={DataCenterConfiguration} />
        </Route>
        <Route path="/alerts" component={Alerts}/>
        <Route path="/help" component={Help}/>
        <Route path="/profile" component={Profile}/>
        <Route path="/profile/:tab" component={Profile}/>
        <Route path="/logs" component={YugawareLogs}/>
        <Route path="/releases" component={Releases}/>
      </Route>
    </Route>
  );
};
