// Copyright (c) YugaByte, Inc.
//
// Author: Nishant Sharma(nishant.sharma@hashedin.com)
//
// This file will hold all the destination list of alerts.

import React, { useEffect, useState } from 'react';
import { Button, DropdownButton, MenuItem } from 'react-bootstrap';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { FlexContainer, FlexShrink } from '../../common/flexbox/YBFlexBox';
import { YBConfirmModal } from '../../modals';
import { YBPanelItem } from '../../panels';
import { AlertDestinationDetails } from './AlertDestinationDetails';

/**
 * This is the header for YB Panel Item.
 */
const header = (onAddAlertDestination) => (
  <>
    <h2 className="table-container-title pull-left">Alert Destinations</h2>
    <FlexContainer className="pull-right">
      <FlexShrink>
        <Button
          bsClass="alert-config-actions btn btn-orange btn-config"
          onClick={() => onAddAlertDestination(true)}
        >
          Add Destination
        </Button>
      </FlexShrink>
    </FlexContainer>
  </>
);

export const AlertDestionations = (props) => {
  const [alertDestionation, setAlertDesionation] = useState([]);
  const [alertDestionationDetails, setAlertDesionationDetails] = useState({});
  const {
    alertDestionations,
    closeModal,
    deleteAlertDestination,
    modal: { showModal, visibleModal },
    onAddAlertDestination,
    setInitialValues,
    showDeleteModal,
    showDetailsModal,
    getAlertReceivers
  } = props;

  const setRsponseObject = () => {
    let result = new Map();

    alertDestionations().then((destinations) => {
      getAlertReceivers().then((receivers) => {
        destinations.forEach((dest) => {
          result.set(dest.uuid, {
            name: dest.name,
            uuid: dest.uuid,
            channels: []
          });

          dest['receivers'].forEach((rx) => {
            const matchedRx = receivers.find((receiver) => receiver.uuid === rx);
            const destination = result.get(dest.uuid);

            destination.channels.push({
              uuid: rx,
              targetType: matchedRx.params.targetType,
              targetName: matchedRx.name,
              webHookURL: matchedRx.params?.webhookUrl,
              recipients: matchedRx.params?.recipients
            });
            result.set(dest.uuid, destination);
          });
        });
        setAlertDesionation(Array.from(result.values()));
      });
    });
  };

  useEffect(() => {
    setRsponseObject();
  }, []);

  /**
   * This method is used to set and pass the destination details to
   * the alert destination modal.
   *
   * @param {object} row Respective row
   */
  const setModalDetails = (row) => {
    const data = row.channels.map((channel) => {
      return {
        targetType: channel.targetType,
        targetName: channel.targetName,
        webHookURL: channel?.webHookURL,
        recipients: channel?.recipients
      };
    });

    setAlertDesionationDetails(data);
  };

  /**
   * This method will help us to delete the respective row record.
   *
   * @param {object} row Respective row data.
   */
  const onDeleteDestination = (row) => {
    deleteAlertDestination(row.uuid).then(() => {
      setRsponseObject();
    });
  };

  /**
   * This method will help us to edit the respective alert destination record.
   *
   * @param {object} row Respective row data.
   */
  const onEditDestination = (row) => {
    const channels = row.channels.map((channel) => {
      return {
        value: channel.uuid,
        label: channel.targetName
      };
    });

    const initialVal = {
      type: 'update',
      uuid: row.uuid,
      ALERT_DESTINATION_NAME: row.name,
      DESTINATION_CHANNEL_LIST: channels
    };

    setInitialValues(initialVal);
    onAddAlertDestination(true);
  };

  /**
   *
   * @param {destination} row
   * @returns Comma seperated cannel targetType.
   */
  const getChannelType = (row) => {
    return row.map((channel) => channel.targetName).join();
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
              setModalDetails(row);
              showDetailsModal();
            }}
          >
            <i className="fa fa-info-circle"></i> Details
          </MenuItem>

          <MenuItem onClick={() => onEditDestination(row)}>
            <i className="fa fa-pencil"></i> Edit Destination
          </MenuItem>

          <MenuItem onClick={() => showDeleteModal(row.name)}>
            <i className="fa fa-trash"></i> Delete Destination
          </MenuItem>

          {
            <YBConfirmModal
              name="delete-alert-destination"
              title="Confirm Delete"
              onConfirm={() => onDeleteDestination(row)}
              currentModal={row.name}
              visibleModal={visibleModal}
              hideConfirmModal={closeModal}
            >
              Are you sure you want to delete {row.name} Alert Destination?
            </YBConfirmModal>
          }
        </DropdownButton>
      </>
    );
  };

  return (
    <>
      <YBPanelItem
        header={header(onAddAlertDestination)}
        body={
          <>
            <BootstrapTable
              className="backup-list-table middle-aligned-table"
              data={alertDestionation}
            >
              <TableHeaderColumn dataField="uuid" isKey={true} hidden={true} />
              <TableHeaderColumn
                dataField="name"
                columnClassName="no-border name-column"
                className="no-border"
              >
                Destinations
              </TableHeaderColumn>
              <TableHeaderColumn
                dataField="channels"
                dataFormat={getChannelType}
                columnClassName="no-border name-column"
                className="no-border"
              >
                Channels
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

      <AlertDestinationDetails
        visible={showModal && visibleModal === 'alertDestinationDetailsModal'}
        onHide={closeModal}
        details={alertDestionationDetails}
      />
    </>
  );
};
