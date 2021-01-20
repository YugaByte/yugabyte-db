import React, { FC } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { KeyValueInput } from '../../../../uikit/KeyValueInput/KeyValueInput';
import { DBConfigFormValue } from '../../steps/db/DBConfig';
import { I18n, translate } from '../../../../uikit/I18n/I18n';
import { ControllerRenderProps } from '../../../../helpers/types';
import { FlagsObject } from '../../../../helpers/dtos';
import './TServerFlagsField.scss';

interface TServerFlagsFieldProps {
  disabled: boolean;
}

export const TServerFlagsField: FC<TServerFlagsFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<DBConfigFormValue>();

  return (
    <div className="tserver-flags-field">
      <div className="tserver-flags-field__head">
        <I18n className="tserver-flags-field__title">YB-TServer</I18n>
        <div className="tserver-flags-field__line" />
      </div>
      <Controller
        control={control}
        name="tserverGFlags"
        render={({ value, onChange }: ControllerRenderProps<FlagsObject>) => (
          <KeyValueInput
            placeholderKey={translate('Flag')}
            placeholderValue={translate('Value')}
            value={value}
            onChange={onChange}
            disabled={disabled}
          />
        )}
      />
    </div>
  );
};
