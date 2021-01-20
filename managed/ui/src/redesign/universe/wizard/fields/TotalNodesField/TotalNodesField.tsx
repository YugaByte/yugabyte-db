import _ from 'lodash';
import React, { FC } from 'react';
import { useUpdateEffect } from 'react-use';
import { Controller, useFormContext } from 'react-hook-form';
import { Input } from '../../../../uikit/Input/Input';
import { CloudConfigFormValue } from '../../steps/cloud/CloudConfig';
import { ControllerRenderProps } from '../../../../helpers/types';
import './TotalNodesField.scss';

const FIELD_NAME = 'totalNodes';

export const TotalNodesField: FC = () => {
  const { control, getValues, setValue, watch, trigger } = useFormContext<CloudConfigFormValue>();

  const placements = watch('placements');
  const autoPlacement = watch('autoPlacement');
  const replicationFactor = watch('replicationFactor');

  useUpdateEffect(() => {
    const totalNodes = getValues(FIELD_NAME);

    if (autoPlacement) {
      // ensure totalNodes always greater or equal to replicationFactor
      if (totalNodes < replicationFactor) {
        setValue(FIELD_NAME, replicationFactor);
      }
    } else {
      // update total nodes and trigger placements validation
      const newTotalNodes = _.sumBy(placements, 'numNodesInAZ') || 0;
      setValue(FIELD_NAME, newTotalNodes);
      trigger('placements');
    }
  }, [replicationFactor, autoPlacement, placements]);

  return (
    <div className="universe-total-nodes-field">
      <Controller
        control={control}
        name={FIELD_NAME}
        render={({ value, onChange, onBlur }: ControllerRenderProps<number>) => (
          <Input
            type="number"
            disabled={!autoPlacement}
            value={value}
            onChange={(event) => {
              // ensure totalNodes always greater or equal to replicationFactor
              const totalNodes = Number(event.target.value.replace(/\D/g, ''));
              if (totalNodes >= replicationFactor) onChange(totalNodes);
            }}
            onBlur={onBlur}
          />
        )}
      />
    </div>
  );
};
