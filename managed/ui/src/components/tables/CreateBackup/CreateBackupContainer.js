// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { CreateBackup } from '../';
import { createTableBackup, createTableBackupResponse } from '../../../actions/tables';
import { createUniverseBackup, createUniverseBackupResponse,
  fetchUniverseBackups, fetchUniverseBackupsResponse } from '../../../actions/universe';
import { isNonEmptyArray, isNonEmptyObject } from "../../../utils/ObjectUtils";

const mapDispatchToProps = (dispatch) => {
  return {
    createTableBackup: (universeUUID, tableUUID, payload) => {
      Object.keys(payload).forEach((key) => { if (typeof payload[key] === 'string' || payload[key] instanceof String) payload[key] = payload[key].trim(); });
      dispatch(createTableBackup(universeUUID, tableUUID, payload)).then((response) => {
        dispatch(createTableBackupResponse(response.payload));
      });
    },
    createUniverseBackup: (universeUUID, payload) => {
      Object.keys(payload).forEach((key) => { if (typeof payload[key] === 'string' || payload[key] instanceof String) payload[key] = payload[key].trim(); });
      dispatch(createUniverseBackup(universeUUID, payload)).then((response) => {
        dispatch(createUniverseBackupResponse(response.payload));
        if (!response.error) {
          dispatch(fetchUniverseBackups(universeUUID))
          .then((response) => {
            dispatch(fetchUniverseBackupsResponse(response.payload));
          });
        }
      });
    }
  };
};

function mapStateToProps(state, ownProps) {
  const { customer: { configs }, tables: { universeTablesList } } = state;
  const storageConfigs = configs.data.filter( (config) => config.type === "STORAGE");
  const initialFormValues = {
    enableSSE: false,
    transactionalBackup: false,
    parallelism: 8
  };

  if (isNonEmptyObject(ownProps.tableInfo)) {
    initialFormValues.backupTableUUID = ownProps.tableInfo.tableID;
  } else if (isNonEmptyArray(universeTablesList.data)) {
    initialFormValues.backupTableUUID = universeTablesList.tableUUID;
  }

  if (isNonEmptyArray(storageConfigs)) {
    initialFormValues.storageConfigUUID = {value: storageConfigs[0].configUUID, label: storageConfigs[0].name + " Storage"};
  }

  const tablesList = state.tables.universeTablesList.filter(table => !table.isIndexTable);
  return {
    storageConfigs: storageConfigs,
    universeDetails: state.universe.currentUniverse.data.universeDetails,
    universeTables: tablesList,
    initialValues: initialFormValues
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(CreateBackup);
