// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { DropdownButton } from 'react-bootstrap';

import { YBPanelItem } from '../../../components/panels';
import { YBButton, YBTextInput } from '../../../components/common/forms/fields';
import { TableAction } from '../../../components/tables';
import { YBLoadingCircleIcon } from '../../../components/common/indicators';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { isAvailable } from '../../../utils/LayoutUtils';
import { showOrRedirect } from '../../../utils/LayoutUtils';

import './ReleaseList.scss';

const versionReg = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)-(\S*)$/;
// Sort descending
const sortVersion = (a, b) => {
  const matchA = versionReg.exec(a);
  const matchB = versionReg.exec(b);

  // Both are proper version strings
  if (matchA && matchB) {
    for (let i = 1; i < matchA.length - 1; i++) {
      const groupA = matchA[i];
      const groupB = matchB[i];
      if (groupA !== groupB) {
        return parseInt(groupB, 10) - parseInt(groupA, 10);
      }
    }
    const tagA = matchA[5];
    const tagB = matchB[5];
    if (tagA !== tagB) {
      // Sort b{n} tags before custom build tags like mihnea, sergei, or 9b180349b
      const matchTagA = tagA.match(/b(\d+)/);
      const matchTagB = tagB.match(/b(\d+)/);
      if (matchTagA && matchTagB) {
        return parseInt(matchTagB[1], 10) - parseInt(matchTagA[1], 10);
      } else if (matchTagA || matchTagB) {
        return !!matchTagB - !!matchTagA; // Boolean - Boolean returns -1, 0, or 1
      }
      return tagA.localeCompare(tagB);
    }
    return 0;
  } else if (matchA || matchB) {
    return !!matchB - !!matchA; // Boolean - Boolean returns -1, 0, or 1
  } else {
    return a.localeCompare(b); // Sort alphabetically ascending
  }
};

export default class ReleaseList extends Component {
  static defaultProps = {
    title: 'Releases'
  };

  state = {
    searchResults: null,
    searchTerm: ''
  };

  componentDidMount() {
    this.props.getYugaByteReleases();
  }

  refreshRelease = () => {
    this.props.refreshYugaByteReleases();
    this.props.getYugaByteReleases();
  };

  onModalSubmit = () => {
    this.props.getYugaByteReleases();
  };

  onSearchVersions = (term) => {
    const { releases } = this.props;
    if (!term) {
      this.setState({ searchResults: null, searchTerm: '' });
    } else {
      this.setState({
        searchResults: Object.keys(releases.data).filter((x) => x.indexOf(term) > -1),
        searchTerm: term
      });
    }
  };

  formatReleaseState = (item) => {
    switch (item) {
      case 'ACTIVE':
        return <div className="state-pill state-pill--success">{item}</div>;
      case 'DISABLED':
        return <div className="state-pill state-pill--danger">{item}</div>;
      default:
        return <div className="state-pill state-pill--secondary">{item}</div>;
    }
  }

  render() {
    const {
      releases,
      title,
      customer: { currentCustomer }
    } = this.props;
    const { searchTerm, searchResults } = this.state;
    showOrRedirect(currentCustomer.data.features, 'main.releases');

    if (getPromiseState(releases).isLoading() || getPromiseState(releases).isInit()) {
      return <YBLoadingCircleIcon size="medium" />;
    }
    let releaseStrList = [];
    if (searchResults != null) {
      releaseStrList = searchResults;
    } else if (releases.data) {
      releaseStrList = Object.keys(releases.data).sort(sortVersion);
    }
    const releaseInfos = releaseStrList.map((version) => {
      const releaseInfo = releases.data[version];
      releaseInfo.version = version;
      return releaseInfo;
    });

    const rowClassNameFormat = function (row, rowIdx) {
      return 'td-column-' + row.state.toLowerCase();
    };
    const self = this;

    const formatActionButtons = function (item, row) {
      let allowedActions = null;
      switch (item) {
        case 'ACTIVE':
          allowedActions = ['DISABLE', 'DELETE'];
          break;
        case 'DISABLED':
          allowedActions = ['DELETE', 'ACTIVE'];
          break;
        default:
          break;
      }
      if (!allowedActions) {
        return;
      }

      return (
        <DropdownButton
          className="btn btn-default"
          title="Actions"
          id="bg-nested-dropdown"
          pullRight
        >
          {allowedActions.map((action, idx) => {
            const actionType = action.toLowerCase() + '-release';
            return (
              <TableAction
                key={action + '-' + idx}
                currentRow={row}
                actionType={actionType}
                onModalSubmit={self.onModalSubmit}
                disabled={!isAvailable(currentCustomer.data.features, 'universes.actions')}
              />
            );
          })}
        </DropdownButton>
      );
    };

    return (
      <YBPanelItem
        header={
          <div>
            <div className="pull-left">
              <YBTextInput
                placeHolder="Search versions"
                value={searchTerm}
                onValueChanged={this.onSearchVersions}
              />
            </div>
            <div className="pull-right">
              <div className="release-list-action-btn-group">
                <YBButton
                  btnText={'Refresh'}
                  btnIcon={'fa fa-refresh'}
                  btnClass={'btn btn-orange'}
                  onClick={this.refreshRelease}
                  disabled={!isAvailable(currentCustomer.data.features, 'universes.actions')}
                />
                <TableAction
                  className="table-action"
                  btnClass={'btn-default'}
                  actionType="import-release"
                  isMenuItem={false}
                  onSubmit={self.onModalSubmit}
                  disabled={!isAvailable(currentCustomer.data.features, 'universes.actions')}
                />
              </div>
            </div>
            <h2 className="content-title">{title}</h2>
          </div>
        }
        body={
          <BootstrapTable
            data={releaseInfos}
            className={'release-list-table'}
            trClassName={rowClassNameFormat}
            pagination={true}
          >
            <TableHeaderColumn
              dataField="version"
              isKey={true}
              columnClassName="no-border name-column"
              className="no-border"
              width="120px"
            >
              Version
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="filePath"
              tdStyle={{ whiteSpace: 'normal' }}
              columnClassName="no-border name-column"
              className="no-border"
              width="550px"
            >
              File Path
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="imageTag"
              tdStyle={{ whiteSpace: 'normal' }}
              columnClassName="no-border "
              className="no-border"
              width="120px"
            >
              Registry Path
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="notes"
              tdStyle={{ whiteSpace: 'normal' }}
              columnClassName="no-border name-column"
              className="no-border"
            >
              Release Notes
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="state"
              dataFormat={this.formatReleaseState}
              columnClassName="no-border name-column"
              className="no-border"
              width="150px"
              dataAlign="center"
            >
              State
            </TableHeaderColumn>
            <TableHeaderColumn
              dataField="state"
              dataFormat={formatActionButtons}
              columnClassName={'yb-actions-cell'}
              className="no-border"
              width="120px"
            >
              Actions
            </TableHeaderColumn>
          </BootstrapTable>
        }
      />
    );
  }
}
