// Copyright (c) YugaByte, Inc.
import React from 'react';
import { FormattedDate } from 'react-intl';

import { isValidObject } from './ObjectUtils';
import { YBFormattedNumber } from '../components/common/descriptors';
import { YBLoadingCircleIcon } from '../components/common/indicators';

export function timeFormatter(cell) {
  if (!isValidObject(cell)) {
    return '<span>-</span>';
  } else {
    return (
      <FormattedDate
        value={new Date(cell)}
        year="numeric"
        month="long"
        day="2-digit"
        hour="numeric"
        minute="numeric"
      />
    );
  }
}

export function percentFormatter(cell, row) {
  return <YBFormattedNumber value={cell / 100} formattedNumberStyle={'percent'} />;
}

export function successStringFormatter(cell, row) {
  switch (row.status) {
    case 'Success':
    case 'Completed':
      return (
        <span className="yb-success-color">
          <i className="fa fa-check" /> Completed
        </span>
      );
    case 'Initializing':
      return (
        <span className="yb-pending-color">
          <YBLoadingCircleIcon size="inline" /> Initializing
        </span>
      );
    case 'InProgress':
      return (
        <span className="yb-pending-color">
          <YBLoadingCircleIcon size="inline" /> InProcess
        </span>
      );
    case 'Running':
      return (
        <span className="yb-pending-color">
          <YBLoadingCircleIcon size="inline" />
          Pending ({percentFormatter(row.percentComplete, row)})
        </span>
      );
    case 'Failure':
    case 'Failed':
      return (
        <span className="yb-fail-color">
          <i className="fa fa-warning" /> Failed
        </span>
      );
    default:
      return (
        <span className="yb-fail-color">
          <i className="fa fa-warning" />
          Unknown
        </span>
      );
  }
}

export function alertTypeFormatter(cell, row) {
  switch (row.type) {
    case 'Error':
      return (
        <span className="yb-fail-color">
          <i className="fa fa-warning" /> Error
        </span>
      );
    case 'Warning':
      return (
        <span className="yb-warn-color">
          <i className="fa fa-warning" /> Warning
        </span>
      );
    default:
      return <span>{row.type}</span>;
  }
}
