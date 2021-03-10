// Copyright (c) YugaByte, Inc.
//
// Author: Nishant Sharma(nishant.sharma@hashedin.com)
//
// This file will hold a common list view for the different kind
// of storage configuration.

import React, { useState } from "react";
import { Button, DropdownButton, MenuItem } from "react-bootstrap";
import { BootstrapTable, TableHeaderColumn } from "react-bootstrap-table";
import YBInfoTip from "../../common/descriptors/YBInfoTip";
import { FlexContainer, FlexShrink } from "../../common/flexbox/YBFlexBox";
import { YBConfirmModal } from "../../modals";
import { YBPanelItem } from "../../panels";

// Data format for configuration name column.
const getBackupConfigName = (cell, row) => {
  return row.configName;
};

// Data format for status column.
const getBackupStatus = (cell, row) => {
  return row.inUse ? "Used" : "Not Used";
};

// Data format for backup location column.
const getBackupLocation = (cell, row) => {
  return row.data.BACKUP_LOCATION;
};

// This method will return all the necessary actions
// for the give list.
const foramtConfigActions = (cell, row, operations) => {
  const {
    configUUID,
    inUse,
    name
  } = row;
  const {
    configData,
    onEdit,
    deleteStorageConfig,
    setConfigData,
    showDeleteStorageConfig,
    visibleModal,
    hideDeleteStorageConfig
  } = operations;

  return (
    <DropdownButton
      className="backup-config-actions btn btn-default"
      title="Actions"
      id="bg-nested-dropdown"
      pullRight
    >
      {/* TODO: Under discussion whether we need to enable this or not. */}

      {/* <MenuItem onClick={() => onEdit(row)}>
        <i className="fa fa-pencil"></i> Edit Configuration
      </MenuItem> */}
      <MenuItem
        onClick={() => {
          setConfigData(configUUID);
          showDeleteStorageConfig(name);
        }}
        disabled={inUse}>
        {!inUse &&
          <><i className="fa fa-trash"></i> Delete Configuration</>
        }

        {inUse &&
          <YBInfoTip content="Storage configuration is in use and cannot be deleted until associated resources are removed."
            placement="top"
          >
            <span className="disable-delete">
              <i className="fa fa-ban"></i> Delete Configuration
            </span>
          </YBInfoTip>
        }
      </MenuItem>

      {<YBConfirmModal
          name="delete-storage-config"
          title="Confirm Delete"
          onConfirm={() => deleteStorageConfig(configData)}
          currentModal={"delete" + name + "StorageConfig"}
          visibleModal={visibleModal}
          hideConfirmModal={hideDeleteStorageConfig}
        >
          Are you sure you want to delete {name} Storage Configuration?
        </YBConfirmModal>
      }

      {/* TODO: Need to implement the show universe which is in review list */}
      <MenuItem>
        <i className="fa fa-eye"></i> Show Universes
      </MenuItem>
    </DropdownButton>
  );
};

const header = (currTab, onCreateBackup) => (
  <>
    <h2 className="table-container-title pull-left">Backup List</h2>
    <FlexContainer className="pull-right">
      <FlexShrink>
        <Button
          bsClass="btn btn-orange btn-config"
          onClick={onCreateBackup}
        >
          Create {currTab} Backup
        </Button>
      </FlexShrink>
    </FlexContainer>
  </>
);

const BackupList = (props) => {
  const [ configData, setConfigData ] = useState({});  
  const {
    activeTab,
    data,
    deleteStorageConfig,
    hideDeleteStorageConfig,
    onCreateBackup,
    onEditConfig,
    showDeleteStorageConfig,
    visibleModal
  } = props;
  const currTab = activeTab.toUpperCase();
  const onEdit = onEditConfig;

  return (
    <div>
      <YBPanelItem
        header={header(currTab, onCreateBackup)}
        body={
          <>
            <BootstrapTable
              className="backup-list-table middle-aligned-table"
              data={data}
            >
              <TableHeaderColumn
                dataField="configUUID"
                isKey={true}
                hidden={true}
              />
              <TableHeaderColumn
                dataField="configurationName"
                dataFormat={getBackupConfigName}
                columnClassName="no-border name-column"
                className="no-border"
              >
                Configuration Name
                </TableHeaderColumn>
              <TableHeaderColumn
                dataField="status"
                dataFormat={getBackupStatus}
                columnClassName="no-border name-column"
                className="no-border"
              >
                Status
                </TableHeaderColumn>
              <TableHeaderColumn
                dataField="backupLocation"
                dataFormat={getBackupLocation}
                columnClassName="no-border name-column"
                className="no-border"
              >
                Backup Location
              </TableHeaderColumn>
              <TableHeaderColumn
                dataField="configActions"
                dataFormat={(cell, row) => foramtConfigActions(
                  cell,
                  row,
                  {
                    configData,
                    deleteStorageConfig,
                    hideDeleteStorageConfig,
                    onEdit,
                    setConfigData,
                    showDeleteStorageConfig,
                    visibleModal
                  }
                )}
                columnClassName="yb-actions-cell"
                className="yb-actions-cell"
              >
                Actions
              </TableHeaderColumn>
            </BootstrapTable>
          </>
        }
        noBackground
      />
    </div>
  )
}

export { BackupList }