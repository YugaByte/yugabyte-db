// Copyright YugaByte Inc.

import { connect } from 'react-redux';
import { EncryptionKeyModal } from '../';
import { fetchAuthConfigList, fetchAuthConfigListResponse } from '../../../actions/cloud';
import { setEncryptionKey, setEncryptionKeyResponse } from '../../../actions/universe';

const mapDispatchToProps = (dispatch) => {
  return {
    fetchKMSConfigList: () => {
      return dispatch(fetchAuthConfigList()).then((response) =>
        dispatch(fetchAuthConfigListResponse(response.payload))
      );
    },
    setEncryptionKey: (universeUUID, data) => {
      return dispatch(setEncryptionKey(universeUUID, data)).then((response) => {
        return dispatch(setEncryptionKeyResponse(response.payload));
      });
    }
  };
};

function mapStateToProps(state) {
  return {
    configList: state.cloud.authConfig
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(EncryptionKeyModal);
