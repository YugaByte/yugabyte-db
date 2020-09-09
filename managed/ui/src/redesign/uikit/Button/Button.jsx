import React from 'react';
import './Button.scss';

export const Button = props => {
  const extendedProps = {
    ...props,
    className: `yb-uikit-button ${props.className || ''}`
  };

  return (
    <button {...extendedProps} />
  );
};
