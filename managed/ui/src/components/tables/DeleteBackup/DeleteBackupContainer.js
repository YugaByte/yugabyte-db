// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { DeleteBackup } from '../';
import { deleteBackup, deleteBackupResponse } from '../../../actions/tables';

const mapDispatchToProps = (dispatch) => {
  return {
    deleteBackup: (backupUUID) => {
      return dispatch(deleteBackup(backupUUID)).then((response) => {
        if (!response.error) {
          dispatch(deleteBackupResponse(response));
          return response.payload;
        }
        throw new Error(response.error);
      });
    }
  };
};

export default connect(null, mapDispatchToProps)(DeleteBackup);
