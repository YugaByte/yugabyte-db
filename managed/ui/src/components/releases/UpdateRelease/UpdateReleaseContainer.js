// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { UpdateRelease } from '../../../components/releases';
import { updateYugaByteRelease, updateYugaByteReleaseResponse } from '../../../actions/customers';

const mapDispatchToProps = (dispatch) => {
  return {
    updateYugaByteRelease: (version, payload) => {
      dispatch(updateYugaByteRelease(version, payload)).then((response) => {
        dispatch(updateYugaByteReleaseResponse(response.payload));
      });
    }
  };
};

function mapStateToProps(state, ownProps) {
  return {
    updateRelease: state.customer.updateRelease
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(UpdateRelease);
