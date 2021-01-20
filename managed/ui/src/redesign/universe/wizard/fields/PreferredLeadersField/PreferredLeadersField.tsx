import _ from 'lodash';
import React, { FC, useContext } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { ControllerRenderProps } from '../../../../helpers/types';
import { DBConfigFormValue } from '../../steps/db/DBConfig';
import { Select } from '../../../../uikit/Select/Select';
import { WizardContext } from '../../UniverseWizard';
import { PlacementUI } from '../PlacementsField/PlacementsField';

const getOptionLabel = (option: NonNullable<PlacementUI>): string =>
  `${option.parentRegionName}: ${option.name}`;
const getOptionValue = (option: NonNullable<PlacementUI>): string => option.uuid;

export const PreferredLeadersField: FC = () => {
  const { control } = useFormContext<DBConfigFormValue>();
  const { formData } = useContext(WizardContext);
  const allPlacements = _.compact<NonNullable<PlacementUI>>(formData.cloudConfig.placements);

  return (
    <div className="preferred-leaders-field">
      <Controller
        control={control}
        name="preferredLeaders"
        render={({
          onChange,
          onBlur,
          value: leadersFormValue
        }: ControllerRenderProps<NonNullable<PlacementUI>[]>) => (
          <div>
            <Select<NonNullable<PlacementUI>>
              isSearchable={false}
              isClearable
              isMulti
              getOptionLabel={getOptionLabel}
              getOptionValue={getOptionValue}
              value={leadersFormValue}
              onBlur={onBlur}
              onChange={(selection) => {
                if (selection) {
                  onChange(selection as NonNullable<PlacementUI>[]);
                } else {
                  onChange([]);
                }
              }}
              options={allPlacements}
            />
          </div>
        )}
      />
    </div>
  );
};
