import React, { FC } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { SecurityConfigFormValue } from '../../steps/security/SecurityConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import { Toggle } from '../../../../uikit/Toggle/Toggle';
import { I18n } from '../../../../uikit/I18n/I18n';

interface EnableEncryptionAtRestFieldProps {
  disabled: boolean;
}

export const EnableEncryptionAtRestField: FC<EnableEncryptionAtRestFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<SecurityConfigFormValue>();

  return (
    <Controller
      control={control}
      name="enableEncryptionAtRest"
      render={({ value, onChange }: ControllerRenderProps<boolean>) => (
        <Toggle
          value={value}
          onChange={onChange}
          disabled={disabled}
          description={<I18n>Whether or not to enable encryption at-rest for database</I18n>}
        />
      )}
    />
  );
};
