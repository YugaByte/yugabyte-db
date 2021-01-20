// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { ImportRelease } from '../../../components/releases';
import { importYugaByteRelease, importYugaByteReleaseResponse } from '../../../actions/customers';

const mapDispatchToProps = (dispatch) => {
  return {
    importYugaByteRelease: (payload) => {
      dispatch(importYugaByteRelease(payload)).then((response) => {
        dispatch(importYugaByteReleaseResponse(response.payload));
      });
    }
  };
};

function mapStateToProps(state, ownProps) {
  return {
    initialValues: { version: '' },
    importRelease: state.customer.importRelease
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(ImportRelease);
