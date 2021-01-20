import React, { FC } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { DBConfigFormValue } from '../../steps/db/DBConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import { CommunicationPorts } from '../../../../helpers/dtos';
import { CommunicationPortsEditor } from './CommunicationPortsEditor';
import './CommunicationPortsField.scss';

interface CommunicationPortsFieldProps {
  disabled: boolean;
}

export const CommunicationPortsField: FC<CommunicationPortsFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<DBConfigFormValue>();

  return (
    <div className="communication-ports-field">
      <Controller
        control={control}
        name="communicationPorts"
        render={({ value, onChange }: ControllerRenderProps<CommunicationPorts>) => (
          <CommunicationPortsEditor
            value={value}
            onChange={onChange}
            disabled={disabled}
          />
        )}
      />
    </div>
  );
};
