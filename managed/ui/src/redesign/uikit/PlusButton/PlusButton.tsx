import clsx from 'clsx';
import React, { FC, HTMLAttributes } from 'react';
import { I18n } from '../I18n/I18n';
import './PlusButton.scss';

interface PlusButtonProps extends HTMLAttributes<HTMLElement> {
  disabled?: boolean;
  text: string;
}

export const PlusButton: FC<PlusButtonProps> = ({ disabled, text, className, onClick }) => {
  return (
    <div
      onClick={onClick}
      className={clsx(className, 'yb-uikit-plus-button', {
        'yb-uikit-plus-button--disabled': disabled
      })}
    >
      <I18n>{text}</I18n>
    </div>
  );
};
