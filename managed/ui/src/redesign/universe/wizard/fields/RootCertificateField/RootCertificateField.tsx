import React, { FC } from 'react';
import { useQuery } from 'react-query';
import { Controller, useFormContext } from 'react-hook-form';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { SecurityConfigFormValue } from '../../steps/security/SecurityConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import { Certificate } from '../../../../helpers/dtos';
import { Select } from '../../../../uikit/Select/Select';

const getOptionLabel = (option: Certificate): string => option.label;
const getOptionValue = (option: Certificate): string => option.uuid;

interface RootCertificateFieldProps {
  disabled: boolean;
}

export const RootCertificateField: FC<RootCertificateFieldProps> = ({ disabled }) => {
  const { control } = useFormContext<SecurityConfigFormValue>();
  const { data } = useQuery(QUERY_KEY.getCertificates, api.getCertificates);

  // allocate the first item in the list to the "create new certificate" item
  const certificates: Certificate[] = [
    ({ label: 'Create New Certificate', uuid: null } as unknown) as Certificate,
    ...(data || [])
  ];

  return (
    <Controller
      control={control}
      name="rootCA"
      render={({ onChange, onBlur, value }: ControllerRenderProps<string | null>) => (
        <Select<Certificate>
          isSearchable
          isClearable={false}
          isDisabled={disabled}
          getOptionLabel={getOptionLabel}
          getOptionValue={getOptionValue}
          value={value ? certificates.find((item) => item.uuid === value) : certificates[0]}
          onBlur={onBlur}
          onChange={(selection) => onChange((selection as Certificate).uuid)}
          options={certificates}
        />
      )}
    />
  );
};
