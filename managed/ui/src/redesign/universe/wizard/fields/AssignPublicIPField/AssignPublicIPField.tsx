import React, { FC } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { BaseToggle } from '../../../../uikit/Toggle/BaseToggle';
import { InstanceConfigFormValue } from '../../steps/instance/InstanceConfig';
import { Tooltip } from '../../../../uikit/Tooltip/Tooltip';
import { ControllerRenderProps } from '../../../../helpers/types';
import { I18n } from '../../../../uikit/I18n/I18n';
import './AssignPublicIPField.scss';

interface AssignPublicIPFieldProps {
  disabled: boolean;
}

export const AssignPublicIPField: FC<AssignPublicIPFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<InstanceConfigFormValue>();

  return (
    <div className="assign-public-ip-field">
      <Controller
        control={control}
        name="assignPublicIP"
        render={({ value, onChange }: ControllerRenderProps<boolean>) => (
          <BaseToggle
            value={value}
            disabled={disabled}
            onChange={onChange}
            descriptions={{
              on: (
                <span className="assign-public-ip-field__toggle-on">
                  <I18n>Whether or not to assign a public IP</I18n>
                </span>
              ),
              off: (
                <span className="assign-public-ip-field__toggle-off">
                  <I18n>Recommended to set it up</I18n>
                </span>
              )
            }}
          />
        )}
      />
      <Tooltip>
        <I18n>TODO</I18n>
      </Tooltip>
    </div>
  );
};
