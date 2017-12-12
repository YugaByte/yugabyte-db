// Copyright (c) YugaByte, Inc.

import { reduxForm } from 'redux-form';
import { connect } from 'react-redux';
import { RollingUpgradeForm }  from '../../../common/forms';
import { isNonEmptyObject } from 'utils/ObjectUtils';
import { rollingUpgrade, rollingUpgradeResponse, closeDialog, resetRollingUpgrade,
         fetchUniverseTasks, fetchUniverseTasksResponse, fetchUniverseMetadata, fetchUniverseInfo, fetchUniverseInfoResponse } from '../../../../actions/universe';
import { fetchCustomerTasks, fetchCustomerTasksSuccess, fetchCustomerTasksFailure } from '../../../../actions/tasks';

const mapDispatchToProps = (dispatch) => {
  return {
    /**
     * Dispatch Rolling Upgrade/ Gflag restart to endpoint and handle response.
     * @param values form data payload
     * @param universeUUID UUID of the current Universe
     * @param reset function that sets the value of the form to pristine state
     */
    submitRollingUpgradeForm: (values, universeUUID, reset) => {
      dispatch(closeDialog());
      dispatch(rollingUpgrade(values, universeUUID)).then((response) => {
        dispatch(rollingUpgradeResponse(response.payload));
        // Reset the Rolling upgrade form fields to pristine state,
        // component may be called multiple times within the context of Universe Detail.
        reset();
      });
    },
    fetchCustomerTasks: () => {
      dispatch(fetchCustomerTasks()).then((response) => {
        if (!response.error) {
          dispatch(fetchCustomerTasksSuccess(response.payload));
        } else {
          dispatch(fetchCustomerTasksFailure(response.payload));
        }
      });
    },
    fetchUniverseTasks: (uuid) => {
      dispatch(fetchUniverseTasks(uuid)).then((response) => {
          dispatch(fetchUniverseTasksResponse(response.payload));
        });
    },
    resetRollingUpgrade: () => {
      dispatch(resetRollingUpgrade());
    },

    fetchUniverseMetadata: () => {
      dispatch(fetchUniverseMetadata());
    },

    fetchCurrentUniverse: (universeUUID) => {
      dispatch(fetchUniverseInfo(universeUUID)).then((response) => {
        dispatch(fetchUniverseInfoResponse(response.payload));
      });
    }

  };
};

function mapStateToProps(state, ownProps) {
  const {universe: {currentUniverse}} = state;
  const initalGFlagValues = {};
  if (isNonEmptyObject(currentUniverse) && currentUniverse.data.universeDetails.userIntent) {
    const masterGFlags = currentUniverse.data.universeDetails.userIntent.masterGFlags;
    const tserverGFlags = currentUniverse.data.universeDetails.userIntent.tserverGFlags;
    if(isNonEmptyObject(masterGFlags)) {
      initalGFlagValues.masterGFlags = Object.keys(masterGFlags).map(function(gFlagKey){
        return {name: gFlagKey, value: masterGFlags[gFlagKey]};
      });
    }
    if(isNonEmptyObject(tserverGFlags)) {
      initalGFlagValues.tserverGFlags = Object.keys(tserverGFlags).map(function(gFlagKey){
        return {name: gFlagKey, value: tserverGFlags[gFlagKey]};
      });
    }
  }
  initalGFlagValues.timeDelay = 180;
  initalGFlagValues.rollingUpgrade = false;
  return {
    universe: state.universe,
    softwareVersions: state.customer.softwareVersions,
    initialValues: initalGFlagValues
  };
}

const rollingUpgradeForm = reduxForm({
  form: 'RollingUpgradeForm'
});

export default connect(mapStateToProps, mapDispatchToProps)(rollingUpgradeForm(RollingUpgradeForm));
