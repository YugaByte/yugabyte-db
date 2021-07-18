// Copyright (c) YugaByte, Inc.
//
// Author: Nishant Sharma(nishant.sharma@hashedin.com)
//
// This file will hold all the configuration list of alerts.

import moment from 'moment';
import React, { useEffect, useState } from 'react';
import { DropdownButton, MenuItem } from 'react-bootstrap';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { isNonEmptyArray } from '../../../utils/ObjectUtils';
import { FlexContainer, FlexShrink } from '../../common/flexbox/YBFlexBox';
import { YBConfirmModal } from '../../modals';
import { YBPanelItem } from '../../panels';

/**
 * This is the header for YB Panel Item.
 */
const header = (onCreateAlert, enablePlatformAlert, handleMetricsCall, setInitialValues) => (
  <>
    <h2 className="table-container-title pull-left">Alert Configurations</h2>
    <FlexContainer className="pull-right">
      <FlexShrink>
        <DropdownButton
          className="alert-config-actions btn btn-default"
          title="Create Alert Config"
          id="bg-nested-dropdown"
          pullRight
        >
          <MenuItem
            className="alert-config-list"
            onClick={() => {
              handleMetricsCall('UNIVERSE');
              onCreateAlert(true);
              enablePlatformAlert(false);
              setInitialValues({ ALERT_TARGET_TYPE: 'allUniverses' });
            }}
          >
            <i className="fa fa-globe"></i> Universe Alert
          </MenuItem>

          <MenuItem
            className="alert-config-list"
            onClick={() => {
              handleMetricsCall('CUSTOMER');
              onCreateAlert(true);
              enablePlatformAlert(true);
              setInitialValues({ ALERT_TARGET_TYPE: 'allUniverses' });
            }}
          >
            <i className="fa fa-clone tab-logo" aria-hidden="true"></i> Platform Alert
          </MenuItem>
        </DropdownButton>
      </FlexShrink>
    </FlexContainer>
  </>
);

/**
 * Request payload to get the list of alerts.
 */
const payload = {
  uuids: [],
  name: null,
  active: true,
  targetUuid: null,
  routeUuid: null
};

export const AlertsList = (props) => {
  const [alertList, setAlertList] = useState([]);
  const [alertDestinations, setAlertDestinations] = useState([]);
  const {
    alertConfigs,
    alertUniverseList,
    alertDestionations,
    closeModal,
    deleteAlertConfig,
    enablePlatformAlert,
    handleMetricsCall,
    modal: { visibleModal },
    onCreateAlert,
    showDeleteModal,
    setInitialValues
  } = props;

  useEffect(() => {
    alertConfigs(payload).then((res) => {
      setAlertList(res);
    });

    alertDestionations().then((res) => {
      setAlertDestinations(res);
    });
  }, []);

  /**
   * This method is used to congifure the routeUUID with its repsective
   * destination name.
   *
   * @param {string} cell Not in-use.
   * @param {object} row Respective details
   */
  const formatRoutes = (cell, row) => {
    return alertDestinations
      .map((route) => {
        return route.uuid === row.routeUUID ? route.name : null;
      })
      .filter((res) => res !== null);
  };

  /**
   * This method is used to get the severity from thresholds object.
   *
   * @param {string} cell Not in-use.
   * @param {object} row Respective details.
   */
  const formatThresholds = (cell, row) => {
    return Object.keys(row.thresholds)
      .map((threshold) => threshold)
      .join();
  };

  /**
   * This method will return the date in format.
   *
   * @param {string} cell Not in-use.
   * @param {object} row Respective row.
   */
  const formatcreatedTime = (cell, row) => {
    return moment(row.createTime).format('MM/DD/yyyy');
  };

  /**
   * This method will help us to edit the details for a respective row.
   *
   * @param {object} row Respective row object.
   */
  const onEditAlertConfig = (row) => {
    row.targetType === 'CUSTOMER' ? enablePlatformAlert(true) : enablePlatformAlert(false);

    // setting up ALERT_DESTINATION_LIST.
    const destination = alertDestinations
      .map((route) => {
        return route.uuid === row.routeUUID
          ? {
              value: route.uuid,
              label: route.name
            }
          : null;
      })
      .filter((res) => res !== null);

    // setting up ALERT_METRICS_CONDITION_POLICY.
    const condition = Object.keys(row.thresholds).map((key) => {
      return {
        _SEVERITY: key,
        _CONDITION: row.thresholds[key].condition,
        _THRESHOLD: row.thresholds[key].threshold
      };
    });

    // setting up ALERT_TARGET_TYPE & ALERT_UNIVERSE_LIST.
    const targetType = row.target.all ? 'allUniverses' : 'selectedUniverses';
    const univerList =
      isNonEmptyArray(row.target.uuids) &&
      row.target.uuids.map((list) => alertUniverseList.find((universe) => universe.value === list));

    // setting up the initial values.
    const initialVal = {
      type: 'update',
      uuid: row.uuid,
      ALERT_CONFIGURATION_NAME: row.name,
      ALERT_CONFIGURATION_DESCRIPTION: row.description,
      ALERT_TARGET_TYPE: targetType,
      ALERT_UNIVERSE_LIST: univerList,
      ALERT_METRICS_CONDITION: row.template,
      ALERT_METRICS_DURATION: row.durationSec,
      ALERT_METRICS_CONDITION_POLICY: condition,
      ALERT_DESTINATION_LIST: destination[0].value
    };

    setInitialValues(initialVal);
    onCreateAlert(true);
  };

  /**
   * This method will help us to delete the respective row record.
   *
   * @param {object} row Respective row data.
   */
  const onDeleteConfig = (row) => {
    deleteAlertConfig(row.uuid).then(() => {
      alertConfigs(payload).then((res) => {
        setAlertList(res);
      });
    });
  };

  // This method will handle all the required actions for the particular row.
  const formatConfigActions = (cell, row) => {
    return (
      <>
        <DropdownButton
          className="backup-config-actions btn btn-default"
          title="Actions"
          id="bg-nested-dropdown"
          pullRight
        >
          <MenuItem
            onClick={() => {
              handleMetricsCall(row.targetType);
              onEditAlertConfig(row);
            }}
          >
            <i className="fa fa-pencil"></i> Edit Alert
          </MenuItem>

          <MenuItem onClick={() => showDeleteModal(row.name)}>
            <i className="fa fa-trash"></i> Delete Alert
          </MenuItem>

          {
            <YBConfirmModal
              name="delete-alert-config"
              title="Confirm Delete"
              onConfirm={() => onDeleteConfig(row)}
              currentModal={row.name}
              visibleModal={visibleModal}
              hideConfirmModal={closeModal}
            >
              Are you sure you want to delete {row.name} Alert Config?
            </YBConfirmModal>
          }
        </DropdownButton>
      </>
    );
  };

  // TODO: This needs to be updated once the real data will come.
  // For now, we're dealing with the mock data.
  return (
    <YBPanelItem
      header={header(onCreateAlert, enablePlatformAlert, handleMetricsCall, setInitialValues)}
      body={
        <>
          <BootstrapTable className="backup-list-table middle-aligned-table" data={alertList}>
            <TableHeaderColumn dataField="uuid" isKey={true} hidden={true} />
            <TableHeaderColumn
              dataField="name"
              columnClassName="no-border name-column"
              className="no-border"
            >
              Name
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="targetType"
              dataSort
              columnClassName="no-border name-column"
              className="no-border"
            >
              Type
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="thresholds"
              dataFormat={formatThresholds}
              columnClassName="no-border name-column"
              className="no-border"
            >
              Severity
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="routeUUID"
              dataFormat={formatRoutes}
              columnClassName="no-border name-column"
              className="no-border"
            >
              Destinations
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="createTime"
              dataFormat={formatcreatedTime}
              dataSort
              columnClassName="no-border name-column"
              className="no-border"
            >
              Created
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="template"
              columnClassName="no-border name-column"
              className="no-border"
            >
              Metric Name
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="configActions"
              dataFormat={(cell, row) => formatConfigActions(cell, row)}
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
  );
};
