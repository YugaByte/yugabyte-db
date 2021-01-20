import React, { FC } from 'react';
import { useQuery } from 'react-query';
import { Controller, useFormContext } from 'react-hook-form';
import { Select } from '../../../../uikit/Select/Select';
import { ErrorMessage } from '../../../../uikit/ErrorMessage/ErrorMessage';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { Provider } from '../../../../helpers/dtos';
import { CloudConfigFormValue } from '../../steps/cloud/CloudConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import './ProvidersField.scss';

// simplified provider object with bare minimum fields needed in UI
export type ProviderUI = Pick<Provider, 'uuid' | 'code'>;

const ERROR_NO_PROVIDER = 'Provider value is required';

const getOptionLabel = (option: Provider): string => option.name;
const getOptionValue = (option: Provider): string => option.uuid;

interface ProvidersFieldProps {
  disabled: boolean;
}

const FIELD_NAME = 'provider';

export const ProvidersField: FC<ProvidersFieldProps> = ({ disabled }) => {
  const { control, errors } = useFormContext<CloudConfigFormValue>();
  const { data } = useQuery(QUERY_KEY.getProvidersList, api.getProvidersList);
  const providersList = data || [];

  return (
    <div className="universe-provider-field">
      <Controller
        control={control}
        name={FIELD_NAME}
        rules={{ required: ERROR_NO_PROVIDER }}
        render={({ onChange, onBlur, value }: ControllerRenderProps<ProviderUI | null>) => (
          <Select<Provider>
            isSearchable={false}
            isClearable
            isDisabled={disabled}
            className={errors[FIELD_NAME]?.message ? 'validation-error' : ''}
            getOptionLabel={getOptionLabel}
            getOptionValue={getOptionValue}
            value={providersList.find((provider) => provider.uuid === value?.uuid) || null}
            onBlur={onBlur}
            onChange={(provider) => {
              if (provider) {
                // other fields watching for provider change by reference, thus trigger onChange() when provider really changed
                if ((provider as Provider).uuid !== value?.uuid) {
                  onChange({
                    uuid: (provider as Provider).uuid,
                    code: (provider as Provider).code
                  });
                }
              } else {
                onChange(null);
              }
            }}
            options={providersList}
          />
        )}
      />
      <ErrorMessage message={errors[FIELD_NAME]?.message} />
    </div>
  );
};
