import clsx from 'clsx';
import React, { FC } from 'react';
import './Button.scss';

interface CustomButtonProps {
  isCTA?: boolean;
  chevronLeft?: boolean;
  chevronRight?: boolean;
}

type ButtonProps = JSX.IntrinsicElements['button'] & CustomButtonProps;

export const Button: FC<ButtonProps> = ({
  isCTA,
  chevronLeft,
  chevronRight,
  children,
  ...props
}) => {
  const extendedProps = {
    ...props,
    className: clsx(props.className, 'yb-uikit-button', {
      'yb-uikit-button--cta': isCTA,
      'yb-uikit-button--chevron-left': chevronLeft,
      'yb-uikit-button--chevron-right': chevronRight
    })
  };

  return <button {...extendedProps}>{children}</button>;
};
