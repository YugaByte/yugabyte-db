// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import { Link } from 'react-router';
import { Image, ProgressBar, ButtonGroup, DropdownButton } from 'react-bootstrap';
import tableIcon from '../images/table.png';
import './ListTables.scss';
import { isNonEmptyArray } from '../../../utils/ObjectUtils';
import { TableAction } from '../../tables';

import { UniverseAction } from '../../universes';
import { YBPanelItem } from '../../panels';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import 'react-bootstrap-table/css/react-bootstrap-table.css';
import _ from 'lodash';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { YBResourceCount } from '../../common/descriptors';
import { isDisabled } from '../../../utils/LayoutUtils';

class TableTitle extends Component {
  render() {
    const { customer: { currentCustomer } } = this.props;
    const currentUniverse = this.props.universe.currentUniverse.data;
    const {numCassandraTables, numRedisTables, numPostgresTables} = this.props;
    return (
      <div className="table-container-title clearfix">
        <div className="pull-left">
          <h2>Tables</h2>
          <div className="table-type-count">
            <Image src={tableIcon} className="table-type-logo"/>
            <YBResourceCount kind="YSQL" size={numPostgresTables}/>
          </div>
          <div className="table-type-count">
            <Image src={tableIcon} className="table-type-logo"/>
            <YBResourceCount kind="YCQL" size={numCassandraTables}/>
          </div>
          <div className="table-type-count">
            <Image src={tableIcon} className="table-type-logo"/>
            <YBResourceCount kind="YEDIS" size={numRedisTables}/>
          </div>
        </div>
        <div className="pull-right">
          <div className="backup-action-btn-group">
            <UniverseAction className="table-action" universe={currentUniverse}
              actionType="toggle-backup" btnClass={"btn-orange"}
              disabled={isDisabled(currentCustomer.data.features, "universes.tableActions")}
            />
          </div>
        </div>
      </div>
    );
  }
}

export default class ListTables extends Component {
  constructor(props) {
    super(props);
    this.state = {'currentView': 'listTables'};
  }

  showListTables = () => {
    this.setState({'currentView': 'listTables'});
  };

  render() {
    const self = this;
    const {tables} = this.props;
    let numCassandraTables = 0;
    let numRedisTables = 0;
    let numPostgresTables = 0;
    if (isNonEmptyArray(self.props.tables.universeTablesList)) {
      self.props.tables.universeTablesList.forEach(function (item, idx) {
        if (item.tableType === "REDIS_TABLE_TYPE") {
          numRedisTables++;
        } else if (item.tableType === "YQL_TABLE_TYPE") {
          numCassandraTables++;
        } else {
          numPostgresTables++;
        }
      });
    }

    if (tables.currentTableView === "list") {
      return (
        <YBPanelItem
          header={
            <TableTitle numRedisTables={numRedisTables}
                        numCassandraTables={numCassandraTables}
                        numPostgresTables={numPostgresTables}
                        {...this.props} />
          }
          body={
            <ListTableGrid {...this.props}/>
          }
        />
      );
    } else {
      return <span/>;
    }
  }
}

class ListTableGrid extends Component {
  render(){
    const self = this;
    const {universe: {universeTasks}, customer: {currentCustomer}} = this.props;
    const currentUniverse = this.props.universe.currentUniverse.data;
    const getTableIcon = function(tableType) {
      if (tableType === "YQL_TABLE_TYPE") {
        return "YCQL";
      } else if (tableType === "REDIS_TABLE_TYPE") {
        return "YEDIS";
      } else {
        return "YSQL";
      }
    };

    const getTableName = function (tableName, data) {
      if (data.status === "success") {
        return <Link to={`/universes/${currentUniverse.universeUUID}/tables/${data.tableID}`}>{tableName}</Link>;
      } else {
        return tableName;
      }
    };

    const formatKeySpace = function(cell) {
      return <div>{cell}</div>;
    };
    const actions_disabled = isDisabled(currentCustomer.data.features, "universes.tableActions");
    const formatActionButtons = function(item, row, disabled) {
      if (!row.isIndexTable) {
        const actions = [
          <TableAction key={`${row.tableName}-backup-btn`} currentRow={row}
                      actionType="create-backup"
                      disabled={actions_disabled} btnClass={"btn-orange"}/>
        ];
        if (getTableIcon(row.tableType) === "YCQL") {
          actions.push([
            <TableAction key={`${row.tableName}-import-btn`} currentRow={row} actionType="import"
                        disabled={actions_disabled} />
          ]);
        }
        return (
          <ButtonGroup>
            <DropdownButton className="btn btn-default" title="Actions" id="bg-nested-dropdown"
                        pullRight>
              {actions}
            </DropdownButton>
          </ButtonGroup>
        );
      }
    };

    const tablePlacementDummyData = {"read": "-", "write": "-"};

    const formatTableStatus = function(item, row) {
      if (item === "success") {
        return <i className="yb-success-color fa fa-check"/>;
      } else if (item === "pending") {
        return (
          <div>
            <span className="yb-orange">Pending {row.percentComplete} % complete</span>
            <ProgressBar className={"pending-action-progress"} now={row.percentComplete}/>
          </div>
        );
      }
      else {
        return <i className="indicator-orange fa fa-times"/>;
      }
    };

    let listItems = [];
    if (isNonEmptyArray(self.props.tables.universeTablesList)) {
      listItems = self.props.tables.universeTablesList.map(function (item, idx) {
        return {
          "keySpace": item.keySpace,
          "tableID": item.tableUUID,
          "tableType": item.tableType,
          "tableName": item.tableName,
          "status": "success",
          "read": tablePlacementDummyData.read,
          "write": tablePlacementDummyData.write,
          "isIndexTable": item.isIndexTable
        };
      });
    }
    const currentUniverseTasks = universeTasks.data[currentUniverse.universeUUID];
    if (getPromiseState(universeTasks).isSuccess() && isNonEmptyArray(currentUniverseTasks)) {
      const pendingTableTasks = currentUniverseTasks.find(function(taskItem){
        return taskItem.target === "Table" && taskItem.status === "Running" && taskItem.percentComplete < 100;
      });
      if (pendingTableTasks) {
        // Split title string to extract table name from the title.
        const pendingTableName = pendingTableTasks.title.replace(/.*:\s*/, '');
        if (listItems.findIndex(lItem => lItem.tableName === pendingTableName) === -1) {
          const pendingTableRow = {
            tableID: pendingTableTasks.id,
            tableType: "YQL_TABLE_TYPE",
            tableName: pendingTableName,
            status: "pending",
            actions: false,
            percentComplete: pendingTableTasks.percentComplete
          };
          listItems.push(pendingTableRow);
        }
      }
    }
    const sortedListItems = _.sortBy(listItems, "tableName");
    const tableListDisplay = (
      <BootstrapTable data={sortedListItems} >
        <TableHeaderColumn dataField="tableID" isKey={true} hidden={true} />
        <TableHeaderColumn dataField={"tableName"} dataFormat={getTableName}
                          columnClassName={"table-name-label yb-table-cell"} className={"yb-table-cell"}>
          Table Name</TableHeaderColumn>
        <TableHeaderColumn dataField={"tableType"} dataFormat={ getTableIcon }
                          columnClassName={"table-type-image-header yb-table-cell"} className={"yb-table-cell"}>
          Table Type</TableHeaderColumn>
        <TableHeaderColumn dataField={"keySpace"}
                          columnClassName={"yb-table-cell"} dataFormat={formatKeySpace}>
          Keyspace</TableHeaderColumn>

        <TableHeaderColumn dataField={"status"}
                          columnClassName={"yb-table-cell"} dataFormat={formatTableStatus}>
          Status</TableHeaderColumn>
        <TableHeaderColumn dataField={"read"}
                          columnClassName={"yb-table-cell"} >
          Read</TableHeaderColumn>
        <TableHeaderColumn dataField={"write"}
                          columnClassName={"yb-table-cell"} >
          Write</TableHeaderColumn>
        <TableHeaderColumn dataField={"actions"} columnClassName={"yb-actions-cell"}
                           dataFormat={formatActionButtons}>
          Actions
        </TableHeaderColumn>
      </BootstrapTable>
    );
    return (
      <Fragment>
        {tableListDisplay}
      </Fragment>
    );
  }
}
