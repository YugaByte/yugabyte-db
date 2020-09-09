// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import {
  isNonEmptyObject,
  isNonEmptyArray,
  isNonEmptyString,
  isEmptyString
} from '../../../utils/ObjectUtils';
import { reset } from 'redux-form';
import  OnPremNodesList from './OnPremNodesList';
import {
  getInstanceTypeList, getInstanceTypeListResponse, getRegionList, getRegionListResponse,
  createNodeInstances, createNodeInstancesResponse, getNodeInstancesForProvider,
  getNodesInstancesForProviderResponse, deleteInstance, deleteInstanceResponse
} from '../../../actions/cloud';
import { fetchUniverseList, fetchUniverseListResponse } from '../../../actions/universe';
import { reduxForm } from 'redux-form';
import { closeUniverseDialog } from '../../../actions/universe';
import { openDialog, closeDialog } from '../../../actions/modal';
import _ from 'lodash';

const mapStateToProps = (state) => {
  return {
    cloud: state.cloud,
    universeList: state.universe.universeList,
    visibleModal: state.modal.visibleModal
  };
};

const mapDispatchToProps = (dispatch, ownProps) => {
  return {
    fetchUniverseList: () => {
      dispatch(fetchUniverseList()).then((response) => {
        dispatch(fetchUniverseListResponse(response.payload));
      });
    },

    getInstanceTypeListItems: (provider) => {
      dispatch(getInstanceTypeList(provider)).then((response) => {
        dispatch(getInstanceTypeListResponse(response.payload));
      });
    },

    getRegionListItems: (provider, isMultiAZ) => {
      dispatch(getRegionList(provider, isMultiAZ)).then((response) => {
        dispatch(getRegionListResponse(response.payload));
      });
    },

    createOnPremNodes: (nodePayloadData, pUUID) => {
      nodePayloadData.forEach(function(nodePayload){
        Object.keys(nodePayload).forEach((zoneUUID, zoneIdx) => {
          const nodesForZone = nodePayload[zoneUUID];
          dispatch(createNodeInstances(zoneUUID, nodesForZone)).then((response) => {
            dispatch(createNodeInstancesResponse(response.payload));
            if (zoneIdx === Object.keys(nodePayload).length -1) {
              dispatch(getNodeInstancesForProvider(pUUID)).then((response) => {
                dispatch(getNodesInstancesForProviderResponse(response.payload));
              });
              dispatch(closeDialog());
              dispatch(closeUniverseDialog());
            }
          });
        });
      });
    },

    showAddNodesDialog() {
      dispatch(openDialog("AddNodesForm"));
    },

    hideAddNodesDialog() {
      dispatch(closeDialog());
      dispatch(closeUniverseDialog());
      dispatch(reset("AddNodeForm"));
    },

    fetchConfiguredNodeList: (pUUID) => {
      dispatch(getNodeInstancesForProvider(pUUID)).then((response) => {
        dispatch(getNodesInstancesForProviderResponse(response.payload));
      });
    },

    deleteInstance: (providerUUID, instanceIP) => {
      dispatch(deleteInstance(providerUUID, instanceIP)).then((response) => {
        dispatch(deleteInstanceResponse(response.payload));
        if (response.payload.status === 200) {
          dispatch(getNodeInstancesForProvider(providerUUID)).then((response) => {
            dispatch(getNodesInstancesForProviderResponse(response.payload));
          });
        }
      });
    },

    showConfirmDeleteModal: () => {
      dispatch(openDialog("confirmDeleteNodeInstance"));
    },

    hideDialog: () => {
      dispatch(closeDialog());
      dispatch(closeUniverseDialog());
    },

  };
};

const validate = values => {
  const errors = {instances: {}};
  if (isNonEmptyObject(values.instances)) {
    Object.keys(values.instances).forEach(function (instanceRowKey) {
      const instanceRowArray = values.instances[instanceRowKey];
      errors.instances[instanceRowKey] = [];
      if (isNonEmptyArray(instanceRowArray)) {
        instanceRowArray.forEach(function(instanceRowItem, instanceRowIdx){
          errors.instances[instanceRowKey][instanceRowIdx] = {};
          if (isNonEmptyString(instanceRowItem.instanceTypeIPs)) {
            const instanceTypeIPs = instanceRowItem.instanceTypeIPs.split(",");
            instanceTypeIPs.forEach(function (ipItem) {
              // Limit length for the case where hostnames are being inputted to protect against
              // UNIX socket limit (a valid IP address will never hit this length limit anyways)
              if (!isNonEmptyString(ipItem) || ipItem.length > 75) {
                errors.instances[instanceRowKey][instanceRowIdx] = {instanceTypeIPs: "Invalid Instance Address"};
              }
            });

            if (!_.get(instanceRowItem, "instanceNames", false) || isEmptyString(instanceRowItem.instanceNames) || instanceRowItem.instanceNames.split(",").length !== instanceTypeIPs.length) {
              errors.instances[instanceRowKey][instanceRowIdx] = {instanceNames: "Invalid Number of Names"};
            } else if (isNonEmptyString(instanceRowItem.instanceNames) && instanceRowItem.instanceNames.split(",").length === instanceTypeIPs.length) {
              instanceRowItem.instanceNames.split(",").forEach(function (instanceName) {
                if (!isNonEmptyString(instanceName)) {
                  errors.instances[instanceRowKey][instanceRowIdx] = {instanceNames: "Invalid Number of Names"};
                }
              });
            }
          }
        });
      }
    });
  }
  return errors;
};

const addNodeForm = reduxForm({
  form: 'AddNodeForm',
  validate
});

export default connect(mapStateToProps, mapDispatchToProps)(addNodeForm(OnPremNodesList));
