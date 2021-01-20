// Copyright YugaByte Inc.

import { connect } from 'react-redux';

import { UniverseDisplayPanel } from '../../../components/panels';
import { closeUniverseDialog, fetchUniverseMetadata } from '../../../actions/universe';
import { openDialog, closeDialog } from '../../../actions/modal';

const mapDispatchToProps = (dispatch) => {
  return {
    showUniverseModal: () => {
      dispatch(openDialog('universeModal'));
    },
    closeUniverseModal: () => {
      dispatch(closeDialog());
      dispatch(closeUniverseDialog());
    },
    fetchUniverseMetadata: () => {
      dispatch(fetchUniverseMetadata());
    }
  };
};

export default connect(null, mapDispatchToProps)(UniverseDisplayPanel);
