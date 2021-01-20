// Copyright YugaByte Inc.

import { NodeActionModal } from '../../universes';
import { connect } from 'react-redux';

import {
  performUniverseNodeAction,
  performUniverseNodeActionResponse
} from '../../../actions/universe';

function mapStateToProps(state) {
  return {
    universe: state.universe
  };
}

const mapDispatchToProps = (dispatch) => {
  return {
    performUniverseNodeAction: (universeUUID, nodeName, actionType) => {
      dispatch(performUniverseNodeAction(universeUUID, nodeName, actionType)).then((response) => {
        dispatch(performUniverseNodeActionResponse(response.payload));
      });
    }
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(NodeActionModal);
