import React, { useState } from 'react';
import _ from 'lodash';
import './ReplicationFactor.scss';

export const ReplicationFactor = ({ values, value: initialValue, onChange, disabled}) => {
  const [selected, setSelected] = useState(initialValue);

  const onClick = val => {
    if (val !== selected) {
      setSelected(val);
      _.attempt(onChange, val);
    }
  };

  return (
    <div className={`replication-factor ${disabled ? 'replication-factor--disabled' : ''}`}>
      {values.map(val => (
        <div
          key={val}
          className={`replication-factor__item ${selected === val ? 'replication-factor__item--selected' : ''}`}
          onClick={() => onClick(val)}
        >
          {val}
        </div>
      ))}
    </div>
  );
};
