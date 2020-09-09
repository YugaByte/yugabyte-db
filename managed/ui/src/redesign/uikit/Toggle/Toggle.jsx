import React, { useRef, useState } from 'react';
import _ from 'lodash';
import { ReactComponent as Checkmark } from './done-24px.svg';
import { ReactComponent as Crossmark } from './close-24px.svg';
import './Toggle.scss';

export const Toggle = ({ value: initialValue, onChange, disabled, customTexts, description }) => {
  const [checked, setChecked] = useState(initialValue || false);
  // render custom texts in hidden div and then use its width to allocate fixed space for value
  const hiddenCustomTexts = useRef(null);

  const toggle = () => {
    const newValue = !checked;
    setChecked(newValue);
    _.attempt(onChange, newValue);
  };

  return (
    <div className={`yb-uikit-toggle ${disabled ? 'yb-uikit-toggle--disabled' : ''}`} onClick={toggle}>
      {customTexts && (
        <div className="yb-uikit-toggle__hidden-custom-texts" ref={hiddenCustomTexts}>
          <div className="yb-uikit-toggle__value">{customTexts.on}</div>
          <div className="yb-uikit-toggle__value">{customTexts.off}</div>
        </div>
      )}
      <div className={`
        yb-uikit-toggle__switch
        ${checked ? 'yb-uikit-toggle__switch--on' : 'yb-uikit-toggle__switch--off'}
        ${disabled ? 'yb-uikit-toggle__switch--disabled' : ''}
      `}>
        <div className="yb-uikit-toggle__value" style={{ width: (hiddenCustomTexts.current || {}).offsetWidth || 'auto' }}>
          {customTexts
            ? (checked ? customTexts.on : customTexts.off)
            : checked ? <Checkmark /> : <Crossmark />
          }
        </div>
        <div className={`
          yb-uikit-toggle__slider
          ${checked ? 'yb-uikit-toggle__slider--on' : 'yb-uikit-toggle__slider--off'}
          ${disabled ? 'yb-uikit-toggle__slider--disabled' : ''}
        `} />
      </div>
      {description && (
        <div className="yb-uikit-toggle__description">
          <div className="yb-uikit-toggle__triangle" />
          {description}
        </div>
      )}
    </div>
  );
};
