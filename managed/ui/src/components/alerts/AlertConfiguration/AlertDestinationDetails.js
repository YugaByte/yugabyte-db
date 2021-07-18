// Copyright (c) YugaByte, Inc.
//
// Author: Nishant Sharma(nissharma@deloitte.com)
//
// This file will hold all the details related to the channels
// for the respective destination.

import React from 'react';
import { isNonEmptyObject } from '../../../utils/ObjectUtils';
import { YBModal } from '../../common/forms/fields';

export const AlertDestinationDetails = ({ details, visible, onHide }) => {
  let showList = [];
  const showDetails = (details) => {
    const len = details.length;
    const hr = len > 1 && <hr />;
    details.forEach((detail, i) => {
      const commonDetails = (
        <>
          <li>
            <label>Target Type:</label>
            <div>{detail.targetType}</div>
          </li>
          <li>
            <label>Target Name:</label>
            <div>{detail.targetName}</div>
          </li>
        </>
      );
      const slackDetails = (
        <>
          {commonDetails}
          <li>
            <label>WebHookURL:</label>
            <div>{detail.webHookURL}</div>
          </li>
          {hr}
        </>
      );
      const emailDetails = (
        <>
          {commonDetails}
          <li>
            <label>Recipient:</label>
            <div>{detail.recipients}</div>
          </li>
          {hr}
        </>
      );

      showList.push(
        <ul key={i} className="cert-details-modal__list">
          {detail.targetType === 'Slack' ? slackDetails : emailDetails}
        </ul>
      );
    });
  };

  return (
    <div className="cert-details-modal">
      <YBModal
        title="Channel Details"
        visible={visible}
        onHide={onHide}
        submitLabel={'Close'}
        onFormSubmit={onHide}
      >
        {isNonEmptyObject(details) && showDetails(details)}
        {showList}
      </YBModal>
    </div>
  );
};
