import React, { useState, useEffect, useRef, useLayoutEffect } from 'react';
// Can't use `useLocation` hook because this component is the child
// of a component that calls withRouter: https://github.com/ReactTraining/react-router/issues/7015
import { withRouter } from 'react-router';
import { useSelector } from 'react-redux';
import { Dropdown, MenuItem, Alert } from 'react-bootstrap';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import Highlight from 'react-highlight';
import { YBPanelItem } from '../panels';
import { QueryInfoSidePanel } from './QueryInfoSidePanel';
import { YBButtonLink } from '../common/forms/fields';
import { useApiQueriesFetch, filterBySearchTokens } from './queriesHelper';
import { YBLoadingCircleIcon } from '../common/indicators';

import './LiveQueries.scss';

export const dropdownColKeys = {
  'Node Name': {
    value: 'nodeName',
    type: 'string'
  },
  'Private IP': {
    value: 'privateIp',
    type: 'string'
  },
  Keyspace: {
    value: 'keyspace',
    type: 'string'
  },
  'DB Name': {
    value: 'dbName',
    type: 'string'
  },
  Query: {
    value: 'query',
    type: 'string'
  },
  'Elapsed Time': {
    value: 'elapsedMillis',
    type: 'number'
  },
  Type: {
    value: 'type',
    type: 'string'
  },
  'Client Host': {
    value: 'clientHost',
    type: 'string'
  },
  'Client Port': {
    value: 'clientPort',
    type: 'string'
  },
  'Session Status': {
    value: 'sessionStatus',
    type: 'string'
  },
  'Client Name': {
    value: 'appName',
    type: 'string'
  }
};

const TAB_KEY_CODE = 9;
const ENTER_KEY_CODE = 13;

const LiveQueriesComponent = ({ location }) => {
  const [type, setType] = useState('');
  const [showAutoComplete, setShowAutoComplete] = useState(false);
  const [searchTokens, setSearchTokens] = useState([]);
  const [selectedRow, setSelectedRow] = useState([]);
  const searchInput = useRef(null);
  const currentUniverse = useSelector((state) => state.universe.currentUniverse);
  const universeUUID = currentUniverse?.data?.universeUUID;
  const { ycqlQueries, ysqlQueries, loading, errors, getLiveQueries } = useApiQueriesFetch({
    universeUUID
  });
  const [searchText, setSearchText] = useState('');
  const [searchDropdownLeftPx, setSearchDropdownLeft] = useState(0);

  useEffect(() => {
    if (location.search) {
      if ('nodeName' in location.query) {
        setSearchTokens([
          {
            label: 'Node Name',
            key: 'nodeName',
            value: location.query['nodeName']
          }
        ]);
      }
    }
  }, []);

  useEffect(() => {
    const searchDropdownHandler = (ev) => {
      const searchBarEl = document.getElementById('query-search-bar');
      if (searchBarEl && !searchBarEl.contains(ev.target)) {
        setShowAutoComplete(false);
      }
    };
    document.addEventListener('click', searchDropdownHandler);

    return () => {
      document.removeEventListener('click', searchDropdownHandler);
    };
  }, [currentUniverse]);

  useEffect(() => {
    // Default to showing YSQL if YSQL tables are present
    if (!type) {
      if (ysqlQueries.length) {
        setType('YSQL');
      } else if (ycqlQueries.length) {
        setType('YCQL');
      }
    }
  }, [ycqlQueries, ysqlQueries]);

  // Gets the location of searchInput element and sets left pixels
  useLayoutEffect(() => {
    if (searchInput && document.getElementById('query-search-bar')) {
      setSearchDropdownLeft(
        searchInput.current.getBoundingClientRect().left -
          document.getElementById('query-search-bar').getBoundingClientRect().left -
          15
      );
    } else {
      setSearchDropdownLeft(0);
    }
  }, [searchInput, searchTokens]);

  const getTserverLink = (cell, row) => {
    return (
      <a href={`http://${row.privateIp}/`} title={cell} target="_blank">
        {cell}
      </a>
    );
  };

  // For overriding Bootstrap toolbar elements and inserting
  // custom CSS classes
  const renderTableToolbar = ({ components }) => {
    return <div className="toolbar-container">{components.searchPanel}</div>;
  };

  // When user clicks autosuggested column name in dropdown
  const handleTokenClick = (e) => {
    setSearchText(`${e.target.innerText}:`);
    searchInput.current.focus();
  };

  const handleKeyPress = (ev, search) => {
    if ((ev.keyCode === TAB_KEY_CODE || ev.keyCode === ENTER_KEY_CODE) && searchText) {
      const separatorIndex = searchText.indexOf(':');
      if (separatorIndex > -1 && searchText.substring(0, separatorIndex) in dropdownColKeys) {
        setSearchTokens([
          ...searchTokens,
          {
            key: dropdownColKeys[searchText.substring(0, separatorIndex)].value,
            label: searchText.substring(0, separatorIndex),
            value: searchText.substring(separatorIndex + 1)
          }
        ]);
        setSearchText('');
      } else {
        setSearchTokens([...searchTokens, { value: searchText }]);
        search(searchText);
        setSearchText('');
      }
      ev.preventDefault();
    }
  };

  const renderCustomSearchPanel = ({ placeholder, search, clearBtnClick }) => {
    return (
      <div id="query-search-bar" className="search-bar-container">
        <div className="search-bar">
          {searchTokens.map((token, idx) => (
            <span className="chip" key={`token-${token.key}-${idx}`}>
              {token.label && <span className="key">{token.label}: </span>}
              <span className="value">{token.value}</span>
              <i
                className="fa fa-times-circle remove-chip"
                onClick={() => {
                  let newTokens = [...searchTokens];
                  newTokens.splice(idx, 1);
                  setSearchTokens(newTokens);
                }}
              />
            </span>
          ))}
          <input
            placeholder={placeholder}
            value={searchText}
            ref={searchInput}
            onChange={(ev) => {
              setSearchText(ev.target.value);
            }}
            onKeyDown={(ev) => handleKeyPress(ev, search)}
            onFocus={() => setShowAutoComplete(true)}
          />
          {searchText && (
            <i
              className="fa fa-times"
              onClick={() => {
                setSearchText('');
                clearBtnClick();
              }}
            />
          )}
        </div>
        {showAutoComplete && !searchText.trim() && (
          <div
            className="autocomplete-wrapper"
            onClick={handleTokenClick}
            style={{
              left: `${searchDropdownLeftPx}px`
            }}
          >
            <ul>
              <li data-col-key="nodeName">Node Name</li>
              <li data-col-key="privateIp">Private IP</li>
              <li data-col-key={type === 'YCQL' ? 'keyspace' : 'dbName'}>
                {type === 'YCQL' ? 'Keyspace' : 'DB Name'}
              </li>
              {type === 'YSQL' && <li data-col-key="sessionStatus">Session Status</li>}
              <li data-col-key="query">Query</li>
              <li data-col-key="elapsedMillis">Elapsed Time</li>
              {type === 'YSQL' ? (
                <li data-col-key="appName">Client Name</li>
              ) : (
                <li data-col-key="type">Type</li>
              )}
              <li data-col-key="clientHost">Client Host</li>
              <li data-col-key="clientPort">Client Port</li>
            </ul>
          </div>
        )}
      </div>
    );
  };

  const getQueryStatement = (cell) => {
    return (
      <div className="query-container">
        <Highlight className="sql">{cell}</Highlight>
      </div>
    );
  };

  const handleRowSelect = (row, isSelected, e) => {
    if (isSelected) {
      setSelectedRow([row.id]);
    } else if (!isSelected && row.id === selectedRow[0].id) {
      setSelectedRow([]);
    }
    return true;
  };

  const displayedQueries =
    type === 'YSQL'
      ? filterBySearchTokens(ysqlQueries, searchTokens)
      : filterBySearchTokens(ycqlQueries, searchTokens);

  let failedQueries = null;
  if (type === 'YSQL') {
    if (errors.ysql > 0) {
      const percentFailed = parseFloat(errors.ysql) / (errors.ysql + ysqlQueries.length);
      failedQueries = (
        <Alert bsStyle={percentFailed > 0.8 ? 'danger' : 'warning'}>
          Number of failed queries: {errors.ysql}/{errors.ysql + ysqlQueries.length}
        </Alert>
      );
    }
  } else if (type === 'YCQL') {
    if (errors.ycql > 0) {
      const percentFailed = parseFloat(errors.ycql) / (errors.ycql + ycqlQueries.length);
      failedQueries = (
        <Alert bsStyle={percentFailed > 0.8 ? 'danger' : 'warning'}>
          Number of failed queries: {errors.ycql}/{errors.ycql + ycqlQueries.length}
        </Alert>
      );
    }
  }

  return (
    <div className="live-queries">
      <YBPanelItem
        header={
          <div className="live-queries__container-title clearfix spacing-top">
            <div className="pull-left">
              <h2 className="content-title pull-left">
                Live Queries
                {loading && (
                  <span className="live-queries__loading-indicator">
                    <YBLoadingCircleIcon size="small" />
                  </span>
                )}
              </h2>
            </div>
            {failedQueries}
            <div className="pull-right">
              <YBButtonLink
                btnIcon="fa fa-refresh"
                btnClass="btn btn-default refresh-btn"
                onClick={() => getLiveQueries()}
              />
              <div>
                <div className="live-queries__dropdown-label">Show live queries</div>
                <Dropdown id="queries-filter-dropdown" pullRight={true}>
                  <Dropdown.Toggle>
                    <i className="fa fa-database"></i>&nbsp;
                    {type}
                  </Dropdown.Toggle>
                  <Dropdown.Menu>
                    <MenuItem key="YCQL" active={type === 'YCQL'} onClick={() => setType('YCQL')}>
                      YCQL
                    </MenuItem>
                    <MenuItem key="YSQL" active={type === 'YSQL'} onClick={() => setType('YSQL')}>
                      YSQL
                    </MenuItem>
                  </Dropdown.Menu>
                </Dropdown>
              </div>
            </div>
          </div>
        }
        body={
          <div className="live-queries__table">
            {type === 'YSQL' && (
              <BootstrapTable
                data={displayedQueries}
                search
                searchPlaceholder="Filter by column or query text"
                multiColumnSearch
                selectRow={{
                  mode: 'checkbox',
                  clickToSelect: true,
                  onSelect: handleRowSelect,
                  selected: selectedRow
                }}
                options={{
                  clearSearch: true,
                  toolBar: renderTableToolbar,
                  searchPanel: renderCustomSearchPanel
                }}
              >
                <TableHeaderColumn dataField="id" isKey={true} hidden={true} />
                <TableHeaderColumn
                  dataField="nodeName"
                  width="200px"
                  dataFormat={getTserverLink}
                  dataSort
                >
                  Node Name
                </TableHeaderColumn>
                <TableHeaderColumn dataField="privateIp" width="200px" dataSort>
                  Private IP
                </TableHeaderColumn>
                <TableHeaderColumn dataField="dbName" width="120px" dataSort>
                  DB Name
                </TableHeaderColumn>
                <TableHeaderColumn dataField="sessionStatus" width="150px" dataSort>
                  Session Status
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="query"
                  width="300px"
                  dataSort
                  dataFormat={getQueryStatement}
                >
                  Query
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="elapsedMillis"
                  width="100px"
                  dataFormat={(cell, row) => `${cell} ms`}
                  dataSort
                  width="180px"
                >
                  Elapsed Time (ms)
                </TableHeaderColumn>
                <TableHeaderColumn dataField="appName" width="200px" dataSort>
                  Client Name
                </TableHeaderColumn>
                <TableHeaderColumn dataField="clientHost" width="150px" dataSort>
                  Client Host
                </TableHeaderColumn>
                <TableHeaderColumn dataField="clientPort" width="100px" dataSort>
                  Client Port
                </TableHeaderColumn>
              </BootstrapTable>
            )}
            {type === 'YCQL' && (
              <BootstrapTable
                data={displayedQueries}
                search
                searchPlaceholder="Filter by column or query text"
                selectRow={{
                  mode: 'checkbox',
                  clickToSelect: true,
                  onSelect: handleRowSelect,
                  selected: selectedRow
                }}
                multiColumnSearch
                options={{
                  clearSearch: true,
                  toolBar: renderTableToolbar,
                  searchPanel: renderCustomSearchPanel
                }}
              >
                <TableHeaderColumn dataField="id" isKey={true} hidden={true} />
                <TableHeaderColumn
                  dataField="nodeName"
                  width="150px"
                  dataFormat={getTserverLink}
                  dataSort
                >
                  Node Name
                </TableHeaderColumn>
                <TableHeaderColumn dataField="privateIp" width="200px" dataSort>
                  Private IP
                </TableHeaderColumn>
                <TableHeaderColumn dataField="keyspace" width="160px" dataSort>
                  Keyspace
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="query"
                  width="350px"
                  dataSort
                  dataFormat={getQueryStatement}
                >
                  Query
                </TableHeaderColumn>
                <TableHeaderColumn
                  dataField="elapsedMillis"
                  width="100px"
                  dataFormat={(cell, row) => `${cell} ms`}
                  dataSort
                  width="180px"
                >
                  Elapsed Time (ms)
                </TableHeaderColumn>
                <TableHeaderColumn dataField="type" width="150px" dataSort>
                  Type
                </TableHeaderColumn>
                <TableHeaderColumn dataField="clientHost" width="150px" dataSort>
                  Client Host
                </TableHeaderColumn>
                <TableHeaderColumn dataField="clientPort" width="100px" dataSort>
                  Client Port
                </TableHeaderColumn>
              </BootstrapTable>
            )}
            {!type && <div>No Live Queries at this time</div>}
          </div>
        }
      />
      <QueryInfoSidePanel
        visible={selectedRow.length}
        onHide={() => setSelectedRow([])}
        data={displayedQueries.find((x) => selectedRow.length && x.id === selectedRow[0])}
      />
    </div>
  );
};

export const LiveQueries = withRouter(LiveQueriesComponent);
