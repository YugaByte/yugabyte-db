// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';

import { ListUniverse } from '../../universes';
import {
  fetchUniverseList,
  fetchUniverseListResponse,
  closeUniverseDialog
} from '../../../actions/universe';
import { openDialog, closeDialog } from '../../../actions/modal';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchUniverseList: () => {
      dispatch(fetchUniverseList()).then((response) => {
        dispatch(fetchUniverseListResponse(response.payload));
      });
    },
    showUniverseModal: () => {
      dispatch(openDialog('universeModal'));
    },
    closeUniverseModal: () => {
      dispatch(closeDialog());
      dispatch(closeUniverseDialog());
    }
  };
};

function mapStateToProps(state, ownProps) {
  return {
    universe: state.universe,
    customer: state.customer
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(ListUniverse);
