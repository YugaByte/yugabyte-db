// Copyright (c) YugaByte, Inc.

import { formValueSelector, reduxForm } from 'redux-form';
import { connect } from 'react-redux';
import { RollingUpgradeForm } from '../../../common/forms';
import {
  fetchCustomerTasks,
  fetchCustomerTasksSuccess,
  fetchCustomerTasksFailure
} from '../../../../actions/tasks';
import {
  rollingUpgrade,
  rollingUpgradeResponse,
  closeUniverseDialog,
  resetRollingUpgrade,
  fetchUniverseTasks,
  fetchUniverseTasksResponse,
  fetchUniverseMetadata,
  fetchUniverseInfo,
  fetchUniverseInfoResponse
} from '../../../../actions/universe';
import { isDefinedNotNull, isNonEmptyObject } from '../../../../utils/ObjectUtils';
import { getPrimaryCluster } from '../../../../utils/UniverseUtils';
import { TASK_LONG_TIMEOUT } from '../../../tasks/constants';
import { getPromiseState } from '../../../../utils/PromiseUtils';

const FORM_NAME = 'RollingUpgradeForm';

const mapDispatchToProps = (dispatch) => {
  return {
    /**
     * Dispatch Rolling Upgrade/ Gflag / TLS restart to endpoint and handle response.
     * @param values form data payload
     * @param universeUUID UUID of the current Universe
     */
    submitRollingUpgradeForm: (values, universeUUID) => {
      dispatch(resetRollingUpgrade()); // reset previous rollingUpgrade() error state, if any
      return dispatch(rollingUpgrade(values, universeUUID)).then((response) => {
        if (!response.error) {
          dispatch(closeUniverseDialog());
        }
        return dispatch(rollingUpgradeResponse(response.payload));
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
  const {
    universe: { currentUniverse }
  } = state;

  const initialValues = {};
  if (isNonEmptyObject(currentUniverse) && isNonEmptyObject(currentUniverse.data.universeDetails)) {
    initialValues.tlsCertificate = currentUniverse.data.universeDetails.rootCA;

    const primaryCluster = getPrimaryCluster(currentUniverse.data.universeDetails.clusters);
    if (isDefinedNotNull(primaryCluster)) {
      const masterGFlags = primaryCluster.userIntent.masterGFlags;
      const tserverGFlags = primaryCluster.userIntent.tserverGFlags;
      if (isNonEmptyObject(masterGFlags)) {
        initialValues.masterGFlags = Object.keys(masterGFlags).map((gFlagKey) => {
          return { name: gFlagKey, value: masterGFlags[gFlagKey] };
        });
      }
      if (isNonEmptyObject(tserverGFlags)) {
        initialValues.tserverGFlags = Object.keys(tserverGFlags).map((gFlagKey) => {
          return { name: gFlagKey, value: tserverGFlags[gFlagKey] };
        });
      }
    }
  }
  initialValues.ybSoftwareVersion = state.customer.softwareVersions[0];
  initialValues.timeDelay = TASK_LONG_TIMEOUT / 1000;
  initialValues.upgradeOption = 'Rolling';
  initialValues.rollingUpgrade = true;

  let certificates = [];
  const allCertificates = state.customer.userCertificates;
  if (getPromiseState(allCertificates).isSuccess()) {
    const rootCert = allCertificates.data.find((item) => item.uuid === initialValues.tlsCertificate);
    // show custom certs with same root cert only
    certificates = allCertificates.data.filter(
      (item) => item.certType === 'CustomCertHostPath' && item.checksum === rootCert?.checksum
    );
  }

  const selector = formValueSelector(FORM_NAME);
  const formValues = selector(state, 'upgradeOption', 'ybSoftwareVersion', 'tlsCertificate');

  return {
    modal: state.modal,
    universe: state.universe,
    softwareVersions: state.customer.softwareVersions,
    initialValues,
    certificates,
    formValues
  };
}

const rollingUpgradeForm = reduxForm({
  form: FORM_NAME,
  enableReinitialize: true // to reinitialize form every time the initialValues prop changes
});

export default connect(mapStateToProps, mapDispatchToProps)(rollingUpgradeForm(RollingUpgradeForm));
