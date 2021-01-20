// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import AuthenticatedComponent from './AuthenticatedComponent';
import {
  fetchUniverseList,
  fetchUniverseListResponse,
  resetUniverseList
} from '../../actions/universe';
import {
  getProviderList,
  getProviderListResponse,
  getSupportedRegionData,
  getSupportedRegionDataResponse,
  getEBSTypeList,
  getEBSTypeListResponse,
  getGCPTypeList,
  getGCPTypeListResponse,
  getAZUTypeList,
  getAZUTypeListResponse,
  listAccessKeysResponse,
  listAccessKeys
} from '../../actions/cloud';
import {
  fetchColumnTypes,
  fetchColumnTypesSuccess,
  fetchColumnTypesFailure
} from '../../actions/tables';
import {
  fetchSoftwareVersions,
  fetchSoftwareVersionsSuccess,
  fetchSoftwareVersionsFailure,
  fetchYugaWareVersion,
  fetchYugaWareVersionResponse,
  fetchCustomerConfigs,
  fetchCustomerConfigsResponse,
  getTlsCertificates,
  getTlsCertificatesResponse,
  insecureLogin,
  insecureLoginResponse
} from '../../actions/customers';
import {
  fetchCustomerTasks,
  fetchCustomerTasksSuccess,
  fetchCustomerTasksFailure
} from '../../actions/tasks';
import { setUniverseMetrics } from '../../actions/universe';
import { queryMetrics } from '../../actions/graph';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchSoftwareVersions: () => {
      dispatch(fetchSoftwareVersions()).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(fetchSoftwareVersionsFailure(response.payload));
        } else {
          dispatch(fetchSoftwareVersionsSuccess(response.payload));
        }
      });
    },

    fetchTableColumnTypes: () => {
      dispatch(fetchColumnTypes()).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(fetchColumnTypesFailure(response.payload));
        } else {
          dispatch(fetchColumnTypesSuccess(response.payload));
        }
      });
    },

    fetchUniverseList: () => {
      dispatch(fetchUniverseList()).then((response) => {
        const startTime = Math.floor(Date.now() / 1000) - 12 * 60 * 60;
        const endTime = Math.floor(Date.now() / 1000);
        const queryParams = {
          metrics: ['tserver_rpcs_per_sec_by_universe'],
          start: startTime,
          end: endTime
        };
        dispatch(queryMetrics(queryParams)).then((response) => {
          if (response.payload.status === 200) {
            dispatch(setUniverseMetrics(response.payload));
          }
        });
        dispatch(fetchUniverseListResponse(response.payload));
      });
    },

    fetchCustomerCertificates: () => {
      dispatch(getTlsCertificates()).then((response) => {
        dispatch(getTlsCertificatesResponse(response.payload));
      });
    },

    getEBSListItems: () => {
      dispatch(getEBSTypeList()).then((response) => {
        dispatch(getEBSTypeListResponse(response.payload));
      });
    },

    getGCPListItems: () => {
      dispatch(getGCPTypeList()).then((response) => {
        dispatch(getGCPTypeListResponse(response.payload));
      });
    },

    getAZUListItems: () => {
      dispatch(getAZUTypeList()).then((response) => {
        dispatch(getAZUTypeListResponse(response.payload));
      });
    },

    getYugaWareVersion: () => {
      dispatch(fetchYugaWareVersion()).then((response) => {
        dispatch(fetchYugaWareVersionResponse(response.payload));
      });
    },

    getProviderListItems: () => {
      dispatch(getProviderList()).then((response) => {
        if (response.payload.status === 200) {
          response.payload.data.forEach((provider) => {
            dispatch(listAccessKeys(provider.uuid)).then((response) => {
              dispatch(listAccessKeysResponse(response.payload));
            });
          });
        }
        dispatch(getProviderListResponse(response.payload));
      });
    },

    getSupportedRegionList: () => {
      dispatch(getSupportedRegionData()).then((response) => {
        dispatch(getSupportedRegionDataResponse(response.payload));
      });
    },
    resetUniverseList: () => {
      dispatch(resetUniverseList());
    },

    fetchCustomerTasks: () => {
      return dispatch(fetchCustomerTasks()).then((response) => {
        if (!response.error) {
          return dispatch(fetchCustomerTasksSuccess(response.payload));
        } else {
          return dispatch(fetchCustomerTasksFailure(response.payload));
        }
      });
    },

    fetchInsecureLogin: () => {
      dispatch(insecureLogin()).then((response) => {
        if (response.payload.status === 200) {
          dispatch(insecureLoginResponse(response));
        }
      });
    },

    fetchCustomerConfigs: () => {
      dispatch(fetchCustomerConfigs()).then((response) => {
        dispatch(fetchCustomerConfigsResponse(response.payload));
      });
    }
  };
};

const mapStateToProps = (state) => {
  return {
    cloud: state.cloud,
    currentCustomer: state.customer.currentCustomer,
    universe: state.universe,
    tasks: state.tasks,
    fetchMetadata: state.cloud.fetchMetadata,
    fetchUniverseMetadata: state.universe.fetchUniverseMetadata
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(AuthenticatedComponent);
