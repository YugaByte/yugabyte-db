// Copyright YugaByte Inc.

import { connect } from 'react-redux';
import { TableDetail } from '..';
import {
  fetchTableDetail,
  fetchTableDetailFailure,
  fetchTableDetailSuccess,
  resetTableDetail
} from '../../../actions/tables';
import {
  fetchUniverseInfo,
  fetchUniverseInfoResponse,
  resetUniverseInfo
} from '../../../actions/universe';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchUniverseDetail: (universeUUID) => {
      dispatch(fetchUniverseInfo(universeUUID)).then((response) => {
        dispatch(fetchUniverseInfoResponse(response.payload));
      });
    },

    fetchTableDetail: (universeUUID, tableUUID) => {
      dispatch(fetchTableDetail(universeUUID, tableUUID)).then((response) => {
        if (response.payload.status !== 200) {
          dispatch(fetchTableDetailFailure(response.payload));
        } else {
          dispatch(fetchTableDetailSuccess(response.payload));
        }
      });
    },

    resetTableDetail: () => {
      dispatch(resetTableDetail());
    },

    resetUniverseDetail: () => {
      dispatch(resetUniverseInfo());
    }
  };
};

function mapStateToProps(state) {
  return {
    customer: state.customer,
    universe: state.universe,
    tables: state.tables
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(TableDetail);
