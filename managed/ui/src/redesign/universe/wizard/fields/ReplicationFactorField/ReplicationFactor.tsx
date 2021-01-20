import clsx from 'clsx';
import React, { FC } from 'react';
import './ReplicationFactor.scss';

interface ReplicationFactorProps {
  options: number[];
  value: number;
  onChange(value: number): void;
  disabled?: boolean;
}

export const ReplicationFactor: FC<ReplicationFactorProps> = ({
  options,
  value,
  onChange,
  disabled
}) => {
  const onClick = (newValue: number) => {
    if (newValue !== value) onChange(newValue);
  };

  return (
    <div className={clsx('replication-factor', { 'replication-factor--disabled': disabled })}>
      {options.map((option) => (
        <div
          key={option}
          className={clsx('replication-factor__item', {
            'replication-factor__item--selected': value === option
          })}
          onClick={() => onClick(option)}
        >
          {option}
        </div>
      ))}
    </div>
  );
};
