import React, { FC } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { BaseToggle } from '../../../../uikit/Toggle/BaseToggle';
import { CloudConfigFormValue } from '../../steps/cloud/CloudConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import { I18n } from '../../../../uikit/I18n/I18n';
import './ReplicaPlacementToggleField.scss';

interface ReplicaPlacementToggleFieldProps {
  disabled: boolean;
}

export const ReplicaPlacementToggleField: FC<ReplicaPlacementToggleFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<CloudConfigFormValue>();

  return (
    <Controller
      control={control}
      name="autoPlacement"
      render={({ value, onChange }: ControllerRenderProps<boolean>) => (
        <BaseToggle
          value={value}
          disabled={disabled}
          onChange={onChange}
          sliderTexts={{
            on: <I18n>Auto</I18n>,
            off: <I18n>Manual</I18n>
          }}
          sliderClass={{
            on: 'replicap-lacement-toggle-field__toggle-on',
            off: 'replicap-lacement-toggle-field__toggle-off'
          }}
        />
      )}
    />
  );
};
