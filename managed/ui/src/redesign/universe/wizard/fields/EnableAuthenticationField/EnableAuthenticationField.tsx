import React, { FC } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { SecurityConfigFormValue } from '../../steps/security/SecurityConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import { Toggle } from '../../../../uikit/Toggle/Toggle';
import './EnableAuthenticationField.scss';

interface EnableAuthenticationFieldProps {
  disabled: boolean;
}

export const EnableAuthenticationField: FC<EnableAuthenticationFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<SecurityConfigFormValue>();

  return (
    <div className="enable-authentication-field">
      <Controller
        control={control}
        name="enableAuthentication"
        render={({ value, onChange }: ControllerRenderProps<boolean>) => (
          <Toggle value={value} onChange={onChange} disabled={disabled} />
        )}
      />
    </div>
  );
};
