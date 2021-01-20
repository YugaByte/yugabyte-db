import React, { FC } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { SecurityConfigFormValue } from '../../steps/security/SecurityConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import { Toggle } from '../../../../uikit/Toggle/Toggle';
import { I18n } from '../../../../uikit/I18n/I18n';

interface EnableNodeToNodeTLSFieldProps {
  disabled: boolean;
}

export const EnableNodeToNodeTLSField: FC<EnableNodeToNodeTLSFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<SecurityConfigFormValue>();

  return (
    <Controller
      control={control}
      name="enableNodeToNodeEncrypt"
      render={({ value, onChange }: ControllerRenderProps<boolean>) => (
        <Toggle
          value={value}
          onChange={onChange}
          disabled={disabled}
          description={
            <I18n>Whether or not to enable TLS Encryption for node to node communication</I18n>
          }
        />
      )}
    />
  );
};
