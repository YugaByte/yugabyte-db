// Copyright YugaByte Inc.

import { connect } from 'react-redux';
import Schedules from './Schedules';
import {
  getSchedules,
  getSchedulesResponse,
  deleteSchedule,
  deleteScheduleResponse
} from '../../actions/customers';
import { fetchUniverseList, fetchUniverseListResponse } from '../../actions/universe';
import { openDialog, closeDialog } from '../../actions/modal';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchUniverseList: () => {
      dispatch(fetchUniverseList()).then((response) => {
        dispatch(fetchUniverseListResponse(response.payload));
      });
    },
    deleteSchedule: (scheduleUUID) => {
      dispatch(deleteSchedule(scheduleUUID)).then((response) => {
        dispatch(deleteScheduleResponse(response.payload));
        if (response.payload.status === 200) {
          dispatch(closeDialog());
          dispatch(getSchedules()).then((response) => {
            dispatch(getSchedulesResponse(response.payload));
          });
        }
      });
    },
    getSchedules: () => {
      dispatch(getSchedules()).then((response) => {
        dispatch(getSchedulesResponse(response.payload));
      });
    },
    showAddCertificateModal: () => {
      dispatch(openDialog('deleteScheduleModal'));
    },
    closeModal: () => {
      dispatch(closeDialog());
    }
  };
};

function mapStateToProps(state) {
  return {
    modal: state.modal,
    customer: state.customer,
    universe: state.universe
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(Schedules);
