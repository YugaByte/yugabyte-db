// Copyright YugaByte Inc.

import { connect } from 'react-redux';
import { Importer } from '../importer';
import {
  importUniverse,
  importUniverseResponse,
  importUniverseReset,
  importUniverseInit
} from '../../actions/universe';

const mapDispatchToProps = (dispatch) => {
  return {
    importUniverse: (values) => {
      dispatch(importUniverseInit());
      dispatch(importUniverse(values)).then((response) => {
        dispatch(importUniverseResponse(response.payload));
      });
    },
    importUniverseReset: () => {
      return dispatch(importUniverseReset());
    }
  };
};

function mapStateToProps(state) {
  return {
    universeImport: state.universe.universeImport,
    customer: state.customer
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(Importer);
