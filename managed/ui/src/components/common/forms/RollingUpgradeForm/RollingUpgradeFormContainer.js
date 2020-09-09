// Copyright (c) YugaByte, Inc.

import { formValueSelector, reduxForm } from 'redux-form';
import { connect } from 'react-redux';
import { RollingUpgradeForm }  from '../../../common/forms';
import { fetchCustomerTasks, fetchCustomerTasksSuccess, fetchCustomerTasksFailure } from '../../../../actions/tasks';
import { closeDialog } from '../../../../actions/modal';
import { rollingUpgrade, rollingUpgradeResponse, closeUniverseDialog, resetRollingUpgrade,
  fetchUniverseTasks, fetchUniverseTasksResponse, fetchUniverseMetadata, fetchUniverseInfo,
  fetchUniverseInfoResponse } from '../../../../actions/universe';
import { isDefinedNotNull, isNonEmptyObject } from "../../../../utils/ObjectUtils";
import { getPrimaryCluster } from "../../../../utils/UniverseUtils";
import { TASK_LONG_TIMEOUT } from '../../../tasks/constants';

const FORM_NAME = "RollingUpgradeForm";

const mapDispatchToProps = (dispatch) => {
  return {
    /**
     * Dispatch Rolling Upgrade/ Gflag restart to endpoint and handle response.
     * @param values form data payload
     * @param universeUUID UUID of the current Universe
     * @param reset function that sets the value of the form to pristine state
     */
    submitRollingUpgradeForm: (values, universeUUID, reset) => {
      dispatch(rollingUpgrade(values, universeUUID)).then((response) => {
        if (!response.error) {
          dispatch(closeDialog());
          dispatch(closeUniverseDialog());
        }
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
  if (isNonEmptyObject(currentUniverse) && isNonEmptyObject(currentUniverse.data.universeDetails)) {
    const primaryCluster = getPrimaryCluster(currentUniverse.data.universeDetails.clusters);
    if (isDefinedNotNull(primaryCluster)) {
      const masterGFlags = primaryCluster.userIntent.masterGFlags;
      const tserverGFlags = primaryCluster.userIntent.tserverGFlags;
      if (isNonEmptyObject(masterGFlags)) {
        initalGFlagValues.masterGFlags = Object.keys(masterGFlags).map(function (gFlagKey) {
          return {name: gFlagKey, value: masterGFlags[gFlagKey]};
        });
      }
      if (isNonEmptyObject(tserverGFlags)) {
        initalGFlagValues.tserverGFlags = Object.keys(tserverGFlags).map(function (gFlagKey) {
          return {name: gFlagKey, value: tserverGFlags[gFlagKey]};
        });
      }
    }
  }
  initalGFlagValues.ybSoftwareVersion = state.customer.softwareVersions[0];
  initalGFlagValues.timeDelay = TASK_LONG_TIMEOUT / 1000;
  initalGFlagValues.upgradeOption = "Rolling";
  initalGFlagValues.rollingUpgrade = true;
  const selector = formValueSelector(FORM_NAME);
  const upgradeOption = selector(state, 'upgradeOption');
  return {
    modal: state.modal,
    universe: state.universe,
    softwareVersions: state.customer.softwareVersions,
    initialValues: initalGFlagValues,
    upgradeOption,
  };
}

const rollingUpgradeForm = reduxForm({
  form: FORM_NAME
});

export default connect(mapStateToProps, mapDispatchToProps)(rollingUpgradeForm(RollingUpgradeForm));
