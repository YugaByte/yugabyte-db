import React from 'react';
import './PlusButton.scss';

export const PlusButton = ({ disabled, text, className }) => {
  return (
    <div className={`
      yb-uikit-plus-button
      ${disabled ? 'yb-uikit-plus-button--disabled' : ''}
      ${className}
    `}>
      {text}
    </div>
  );
};
