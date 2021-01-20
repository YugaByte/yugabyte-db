import React, { FC, ReactNode } from 'react';
import { BaseToggle, BaseToggleProps } from './BaseToggle';

type ToggleProps = Pick<BaseToggleProps, 'value' | 'onChange' | 'disabled'> & {
  description?: ReactNode;
};

// standard toggle without the overload of customizations
export const Toggle: FC<ToggleProps> = ({ value, onChange, disabled, description }) => {
  const newProps: BaseToggleProps = { value, onChange, disabled };

  if (description) {
    newProps.descriptions = {
      on: description,
      off: description
    };
  }

  return <BaseToggle {...newProps} />;
};
