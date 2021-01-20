import React, { FC } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { InstanceConfigFormValue } from '../../steps/instance/InstanceConfig';
import { Input } from '../../../../uikit/Input/Input';
import { ControllerRenderProps } from '../../../../helpers/types';

interface ProfileARNFieldProps {
  disabled: boolean;
}

export const ProfileARNField: FC<ProfileARNFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<InstanceConfigFormValue>();

  return (
    <Controller
      control={control}
      name="awsArnString"
      render={({ value, onChange }: ControllerRenderProps<string | null>) => (
        <Input
          type="text"
          disabled={disabled}
          value={value || ''}
          onChange={(event) => onChange(event.target.value)}
        />
      )}
    />
  );
};
