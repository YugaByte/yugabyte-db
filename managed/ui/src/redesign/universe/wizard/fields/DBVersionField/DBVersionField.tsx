import React, { FC } from 'react';
import { useQuery } from 'react-query';
import { Controller, useFormContext } from 'react-hook-form';
import { Select } from '../../../../uikit/Select/Select';
import { ErrorMessage } from '../../../../uikit/ErrorMessage/ErrorMessage';
import { DBConfigFormValue } from '../../steps/db/DBConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { sortVersionStrings } from '../../../../../utils/ObjectUtils';
import './DBVersionField.scss';

interface DBVersionOption {
  value: string;
  label: string;
}

interface DBVersionFieldProps {
  disabled: boolean;
}

const FIELD_NAME = 'ybSoftwareVersion';
const ERROR_NO_DB_VERSION = 'DB Version value is required';

export const DBVersionField: FC<DBVersionFieldProps> = ({ disabled }) => {
  const { control, errors, getValues, setValue } = useFormContext<DBConfigFormValue>();
  const { data } = useQuery(QUERY_KEY.getDBVersions, api.getDBVersions, {
    onSuccess: (data) => {
      // pre-select first available db version
      const sorted: string[] = sortVersionStrings(data);
      if (!getValues(FIELD_NAME) && sorted.length) {
        setValue(FIELD_NAME, sorted[0]); // intentionally omit validation as field wasn't changed by user
      }
    }
  });

  const dbVersions: DBVersionOption[] = sortVersionStrings(data || []).map((item: string) => ({
    label: item,
    value: item
  }));

  return (
    <div className="db-version-field">
      <Controller
        control={control}
        name={FIELD_NAME}
        rules={{ required: ERROR_NO_DB_VERSION }}
        render={({
          onChange,
          onBlur,
          value: dbVersionFormValue
        }: ControllerRenderProps<string | null>) => (
          <Select<DBVersionOption>
            isSearchable
            isClearable={false}
            isDisabled={disabled}
            className={errors[FIELD_NAME]?.message ? 'validation-error' : ''}
            value={dbVersions.find((item) => item.value === dbVersionFormValue)}
            onBlur={onBlur}
            onChange={(item) => onChange((item as DBVersionOption).value)}
            options={dbVersions}
          />
        )}
      />
      <ErrorMessage message={errors[FIELD_NAME]?.message} />
    </div>
  );
};
