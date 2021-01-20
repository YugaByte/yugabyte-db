// Copyright (c) YugaByte, Inc.

import React, { useState } from 'react';
import { YBButton } from '../common/forms/fields';
import { Highlighter } from '../../helpers/Highlighter';
import copy from 'copy-to-clipboard';

const statsTextMap = {
  nodeName: 'Node Name',
  privateIp: 'Private IP',
  keyspace: 'Keyspace',
  dbName: 'DB Name',
  sessionStatus: 'Session Status',
  elapsedMillis: 'Elapsed Time',
  type: 'Type',
  clientHost: 'Client Host',
  clientPort: 'Client Port',
  queryStartTime: 'Query Start',
  appName: 'Client Name'
};

export const QueryInfoSidePanel = ({ data, visible, onHide }) => {
  const [copied, setCopied] = useState(false);

  const handleCopy = () => {
    copy(data.query);
    setCopied(true);
    setTimeout(() => {
      setCopied(false);
    }, 2000);
  };
  return (
    <div className={`side-panel ${!visible ? ' hidden' : ''}`}>
      <div className="side-panel__header">
        <span className="side-panel__icon--close" onClick={onHide}>
          <i className="fa fa-chevron-right" />
        </span>
        <div className="side-panel__title">DETAILS</div>
      </div>
      {data && (
        <div className="side-panel__content">
          <div className="side-panel__query">
            <Highlighter type="sql" text={data.query} element="pre" />
          </div>
          <div className="copy-btn-container">
            <YBButton
              btnText={copied ? 'Copied!' : 'Copy Statement'}
              btnIcon="fa fa-copy"
              onClick={handleCopy}
            />
          </div>
          <ul className="side-panel__details">
            {Object.keys(data).map((key) => {
              if (key !== 'id' && key !== 'query') {
                return (
                  <li key={`details-${key}`}>
                    <label>
                      <strong>{statsTextMap[key]}</strong>
                    </label>
                    <span>{data[key]}</span>
                  </li>
                );
              }
              return null;
            })}
          </ul>
        </div>
      )}
    </div>
  );
};
