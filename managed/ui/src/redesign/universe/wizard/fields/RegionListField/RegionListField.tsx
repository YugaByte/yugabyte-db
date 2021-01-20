import _ from 'lodash';
import React, { FC } from 'react';
import { useQuery } from 'react-query';
import { Controller, useFormContext } from 'react-hook-form';
import { Select } from '../../../../uikit/Select/Select';
import YBLoadingCircleIcon from '../../../../../components/common/indicators/YBLoadingCircleIcon';
import { ErrorMessage } from '../../../../uikit/ErrorMessage/ErrorMessage';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { Region } from '../../../../helpers/dtos';
import { useDeepCompareUpdateEffect } from '../../../../helpers/hooks';
import { ControllerRenderProps, ValidationErrors } from '../../../../helpers/types';
import { CloudConfigFormValue } from '../../steps/cloud/CloudConfig';
import './RegionListField.scss';

const getOptionLabel = (option: Region): string => option.name;
const getOptionValue = (option: Region): string => option.uuid;

const ERROR_NO_REGIONS = 'Regions value is required';
const validate = (value: Region[]): string | boolean =>
  _.isEmpty(value) ? ERROR_NO_REGIONS : true;

const FIELD_NAME = 'regionList';

export const RegionListField: FC = () => {
  const { control, errors, getValues, setValue, watch } = useFormContext<CloudConfigFormValue>();
  const provider = watch('provider');
  const { isFetching, data } = useQuery(
    [QUERY_KEY.getRegionsList, provider?.uuid],
    api.getRegionsList,
    {
      enabled: !!provider?.uuid, // make sure query won't run when there's no provider defined
      onSuccess: (regions) => {
        // if regions are not selected and there's single region available - automatically preselect it
        if (_.isEmpty(getValues(FIELD_NAME)) && regions.length === 1) {
          setValue(FIELD_NAME, [regions[0].uuid], { shouldValidate: true });
        }
      }
    }
  );
  const regionsList = _.isEmpty(data) ? [] : _.sortBy(data, 'name');
  const regionsListMap = _.keyBy(regionsList, 'uuid');

  // reset selected regions on provider change
  // provider is an object, so track its change by deep comparison + skip unwanted first invocation on mount
  useDeepCompareUpdateEffect(() => {
    setValue(FIELD_NAME, []);
  }, [provider]);

  return (
    <div className="universe-regions-field">
      <Controller
        control={control}
        name={FIELD_NAME}
        rules={{ validate }}
        render={({ onChange, onBlur, value }: ControllerRenderProps<string[]>) => (
          <Select<Region>
            isSearchable
            isClearable
            isMulti
            className={(errors as ValidationErrors)[FIELD_NAME]?.message ? 'validation-error' : ''}
            getOptionLabel={getOptionLabel}
            getOptionValue={getOptionValue}
            value={value.map((region) => regionsListMap[region])}
            onBlur={onBlur}
            onChange={(selection) => {
              onChange(
                _.isEmpty(selection) ? [] : (selection as Region[]).map((item) => item.uuid)
              );
            }}
            options={regionsList}
          />
        )}
      />
      {isFetching && (
        <div className="universe-regions-field__spinner">
          <YBLoadingCircleIcon size="small" />
        </div>
      )}
      <ErrorMessage message={(errors as ValidationErrors)[FIELD_NAME]?.message} />
    </div>
  );
};
