import React, { FC } from 'react';
import { useQuery } from 'react-query';
import { Controller, useFormContext } from 'react-hook-form';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { SecurityConfigFormValue } from '../../steps/security/SecurityConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import { KmsConfig } from '../../../../helpers/dtos';
import { Select } from '../../../../uikit/Select/Select';
import { ErrorMessage } from '../../../../uikit/ErrorMessage/ErrorMessage';
import './KMSConfigField.scss';

const getOptionLabel = (option: KmsConfig): string => option.metadata.name;
const getOptionValue = (option: KmsConfig): string => option.metadata.configUUID;

interface KMSConfigFieldProps {
  disabled: boolean;
}

const FIELD_NAME = 'kmsConfig';
const ERROR_NO_KMS = 'Key Management Service Config is required';

export const KMSConfigField: FC<KMSConfigFieldProps> = ({ disabled }) => {
  const { control, errors } = useFormContext<SecurityConfigFormValue>();
  const { data } = useQuery(QUERY_KEY.getKMSConfigs, api.getKMSConfigs);
  const kmsConfigs: KmsConfig[] = data || [];

  // don't validate when field is disabled and make it required otherwise
  // TODO: reset validation error when field becomes disabled
  const validate = (value: string | null): boolean | string => {
    if (disabled) {
      return true;
    } else {
      return !!value || ERROR_NO_KMS;
    }
  };

  return (
    <div className="kms-config-field">
      <Controller
        control={control}
        name={FIELD_NAME}
        rules={{ validate }}
        render={({ onChange, onBlur, value }: ControllerRenderProps<string | null>) => (
          <Select<KmsConfig>
            isSearchable
            isClearable={false}
            isDisabled={disabled}
            className={errors[FIELD_NAME]?.message ? 'validation-error' : ''}
            getOptionLabel={getOptionLabel}
            getOptionValue={getOptionValue}
            value={kmsConfigs.find((item) => item.metadata.configUUID === value) || null}
            onBlur={onBlur}
            onChange={(selection) => onChange((selection as KmsConfig).metadata.configUUID)}
            options={kmsConfigs}
          />
        )}
      />
      <ErrorMessage message={errors[FIELD_NAME]?.message} />
    </div>
  );
};
