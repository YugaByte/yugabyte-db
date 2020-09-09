import React, { useRef } from 'react';
import './Input.scss';

const changeEvent = new Event('change', { bubbles: true });

export const Input = (props) => {
  const input = useRef();

  const up = () => {
    if (props.disabled) return;

    if (input.current.value) {
      input.current.stepUp();
    } else {
      input.current.value = 0;
    }
    input.current.dispatchEvent(changeEvent);
  };

  const down = () => {
    if (props.disabled) return;

    if (input.current.value) {
      input.current.stepDown();
    } else {
      input.current.value = 0;
    }
    input.current.dispatchEvent(changeEvent);
  };

  return (
    <div className={`yb-uikit-input ${props.className || ''}`}>
      <input ref={input} type="text" {...props} />
      {props.type === 'number' && (
        <div className={`
          yb-uikit-input__number-controls
          ${props.disabled ? 'yb-uikit-input__number-controls--disabled' : ''}
        `}>
          <div className="yb-uikit-input__number-up" onClick={up} />
          <div className="yb-uikit-input__number-delim" />
          <div className="yb-uikit-input__number-down" onClick={down} />
        </div>
      )}
    </div>

  );
};
