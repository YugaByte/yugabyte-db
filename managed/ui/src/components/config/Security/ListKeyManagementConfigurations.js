// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import { Button } from 'react-bootstrap';
import { FlexContainer, FlexShrink } from '../../common/flexbox/YBFlexBox';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { YBPanelItem } from '../../panels';

export class ListKeyManagementConfigurations extends Component {
  render() {
    const { configs, onCreate, onDelete } = this.props;

    const actionList = (item, row) => {
      const { configUUID, in_use } = row.metadata;
      return (
        <Button
          title={'Delete provider'}
          bsClass="btn btn-default btn-config pull-right"
          disabled={in_use}
          onClick={() => onDelete(configUUID)}
        >
          Delete Configuration
        </Button>
      );
    };

    const showConfigProperties = (item, row) => {
      const displayed = [];
      const credentials = row.credentials;
      Object.keys(credentials).forEach((key) => {
        if (key !== 'provider') {
          displayed.push(`${key}: ${credentials[key]}`);
        }
      });
      return (
        <div>
          <a
            onClick={(e) => {
              e.target.classList.add('yb-hidden');
              e.target.parentNode.lastChild.classList.remove('yb-hidden');
              e.preventDefault();
            }}
            href="/"
          >
            Show details
          </a>
          <span title={displayed.join(', ')} className="yb-hidden">
            {displayed.join(', ')}
          </span>
        </div>
      );
    };

    return (
      <div>
        <YBPanelItem
          header={
            <Fragment>
              <h2 className="table-container-title pull-left">List Configurations</h2>
              <FlexContainer className="pull-right">
                <FlexShrink>
                  <Button bsClass="btn btn-orange btn-config" onClick={onCreate}>
                    Create New Config
                  </Button>
                </FlexShrink>
              </FlexContainer>
            </Fragment>
          }
          body={
            <Fragment>
              <BootstrapTable
                data={configs.data}
                className="backup-list-table middle-aligned-table"
              >
                <TableHeaderColumn
                  dataField="metadata"
                  dataFormat={cell => cell.configUUID}
                  isKey={true}
                  hidden={true}
                />
                <TableHeaderColumn
                  dataField="metadata"
                  dataFormat={(cell) => cell.name}
                  dataSort
                  columnClassName="no-border name-column"
                  className="no-border"
                >
                  Name
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="metadata"
                  dataFormat={(cell) => cell.provider}
                  dataSort
                  columnClassName="no-border name-column"
                  className="no-border"
                >
                  Provider
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="base_url"
                  dataFormat={showConfigProperties}
                  columnClassName="no-border name-column"
                  className="no-border"
                >
                  Properties
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="configActions"
                  dataFormat={actionList}
                  columnClassName="no-border name-column no-side-padding"
                  className="no-border"
                />
              </BootstrapTable>
            </Fragment>
          }
          noBackground
        />
      </div>
    );
  }
}
