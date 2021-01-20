// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { YBPanelItem } from '../../panels';
import { showOrRedirect } from '../../../utils/LayoutUtils';
import { alertTypeFormatter } from '../../../utils/TableFormatters';

export default class AlertsList extends Component {
  componentDidMount() {
    this.props.getAlertsList();
  }

  render() {
    const {
      customer: { currentCustomer, alerts }
    } = this.props;
    showOrRedirect(currentCustomer.data.features, 'menu.alerts');

    const tableBodyContainer = { marginBottom: '1%', paddingBottom: '1%' };
    return (
      <div>
        <h2 className="content-title">Alerts</h2>
        <YBPanelItem
          body={
            <BootstrapTable
              data={alerts.alertsList}
              bodyStyle={tableBodyContainer}
              pagination={true}
            >
              <TableHeaderColumn dataField="uuid" isKey={true} hidden={true} />
              <TableHeaderColumn
                dataField="createTime"
                columnClassName="no-border"
                className="no-border"
                dataAlign="left"
                width={'20%'}
              >
                Time
              </TableHeaderColumn>
              <TableHeaderColumn
                dataField="type"
                columnClassName="no-border name-column"
                className="no-border"
                dataFormat={alertTypeFormatter}
                width={'10%'}
              >
                Type
              </TableHeaderColumn>
              <TableHeaderColumn
                dataField="errCode"
                columnClassName="no-border name-column"
                className="no-border"
                width={'20%'}
              >
                Error Code
              </TableHeaderColumn>
              <TableHeaderColumn
                dataField="message"
                columnClassName="no-border name-column"
                className="no-border"
                width={'50%'}
                tdStyle={{ whiteSpace: 'normal' }}
              >
                Message
              </TableHeaderColumn>
            </BootstrapTable>
          }
        />
      </div>
    );
  }
}
