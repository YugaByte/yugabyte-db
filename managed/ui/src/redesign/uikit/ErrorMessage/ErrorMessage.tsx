import React, { FC } from 'react';
import { I18n } from '../I18n/I18n';
import './ErrorMessage.scss';

interface ErrorMessageProps {
  message?: string;
  show?: boolean;
}

export const ErrorMessage: FC<ErrorMessageProps> = ({ message, show = true }) => {
  if (!message || !show) return null;

  return (
    <div className="yb-uikit-field-validation-error">
      <I18n>{message}</I18n>
    </div>
  );
};
