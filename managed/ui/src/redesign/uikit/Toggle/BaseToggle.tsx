import clsx from 'clsx';
import React, { FC, useLayoutEffect, useRef, useState, ReactNode } from 'react';
import { ReactComponent as Checkmark } from './done-24px.svg';
import { ReactComponent as Crossmark } from './close-24px.svg';
import './BaseToggle.scss';

export interface BaseToggleProps {
  value: boolean;
  onChange(value: boolean): void;
  disabled?: boolean;
  // custom texts inside slider instead of icons
  sliderTexts?: {
    on: ReactNode;
    off: ReactNode;
  };
  // to override slider
  sliderClass?: {
    on: string;
    off: string;
  };
  // custom descriptions to the right of toggle
  descriptions?: {
    on: ReactNode;
    off: ReactNode;
  };
}

export const BaseToggle: FC<BaseToggleProps> = ({
  value: checked,
  onChange,
  disabled,
  sliderTexts,
  sliderClass,
  descriptions
}) => {
  // render custom texts in hidden div and then use its width to allocate fixed space in slider for text values
  const hiddenSliderTexts = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState('auto');

  useLayoutEffect(() => {
    if (hiddenSliderTexts.current) {
      setWidth(hiddenSliderTexts.current.offsetWidth + 'px');
    }
  }, []);

  const toggle = () => {
    if (!disabled) {
      const newValue = !checked;
      onChange(newValue);
    }
  };

  return (
    <div
      className={clsx('yb-uikit-toggle', { 'yb-uikit-toggle--disabled': disabled })}
      onClick={toggle}
    >
      {sliderTexts && (
        <div className="yb-uikit-toggle__hidden-slider-texts" ref={hiddenSliderTexts}>
          <div className="yb-uikit-toggle__value">{sliderTexts.on}</div>
          <div className="yb-uikit-toggle__value">{sliderTexts.off}</div>
        </div>
      )}
      <div
        className={clsx('yb-uikit-toggle__slider', {
          'yb-uikit-toggle__slider--disabled': disabled,
          'yb-uikit-toggle__slider--on': checked,
          'yb-uikit-toggle__slider--off': !checked,
          [sliderClass?.on || '']: checked,
          [sliderClass?.off || '']: !checked
        })}
      >
        <div className="yb-uikit-toggle__value" style={{ width }}>
          {sliderTexts
            ? (checked ? sliderTexts.on : sliderTexts.off)
            : (checked ? <Checkmark /> : <Crossmark />)
          }
        </div>
        <div
          className={clsx('yb-uikit-toggle__handle', {
            'yb-uikit-toggle__handle--disabled': disabled,
            'yb-uikit-toggle__handle--on': checked,
            'yb-uikit-toggle__handle--off': !checked
          })}
        />
      </div>
      {descriptions && (
        <div className="yb-uikit-toggle__description">
          <div className="yb-uikit-toggle__triangle" />
          {checked ? descriptions.on : descriptions.off}
        </div>
      )}
    </div>
  );
};
