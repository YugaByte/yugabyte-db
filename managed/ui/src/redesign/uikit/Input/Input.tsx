import clsx from 'clsx';
import React, { FC, useRef } from 'react';
import './Input.scss';

interface CustomInputProps {
  invalid?: string; // <input> is a build-in react DOM element, so its props have to be strings only
}
type InputProps = JSX.IntrinsicElements['input'] & CustomInputProps;

const changeEvent = new Event('change', { bubbles: true });

export const Input: FC<InputProps> = (props) => {
  const input = useRef({} as HTMLInputElement);

  const up = () => {
    if (props.disabled) return;

    if (input.current.value) {
      input.current.stepUp();
    } else {
      input.current.value = '0';
    }
    input.current.dispatchEvent(changeEvent);
  };

  const down = () => {
    if (props.disabled) return;

    if (input.current.value) {
      input.current.stepDown();
    } else {
      input.current.value = '0';
    }
    input.current.dispatchEvent(changeEvent);
  };

  return (
    <div
      className={clsx(props.className, 'yb-uikit-input', {
        'yb-uikit-input--disabled': props.disabled,
        'yb-uikit-input--invalid': props.invalid
      })}
    >
      <input ref={input} type="text" autoComplete="off" {...props} />
      {props.type === 'number' && (
        <div
          className={clsx('yb-uikit-input__number-controls', {
            'yb-uikit-input__number-controls--disabled': props.disabled
          })}
        >
          <div className="yb-uikit-input__number-up" onClick={up} />
          <div className="yb-uikit-input__number-delim" />
          <div className="yb-uikit-input__number-down" onClick={down} />
        </div>
      )}
    </div>
  );
};
