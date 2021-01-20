import _ from 'lodash';
import React, { useContext, FC } from 'react';
import { Col, Row } from 'react-bootstrap';
import { useFormContext, Controller } from 'react-hook-form';
import { WizardContext } from '../../UniverseWizard';
import { I18n } from '../../../../uikit/I18n/I18n';
import { Select } from '../../../../uikit/Select/Select';
import { Input } from '../../../../uikit/Input/Input';
import YBLoadingCircleIcon from '../../../../../components/common/indicators/YBLoadingCircleIcon';
import { CloudConfigFormValue } from '../../steps/cloud/CloudConfig';
import { AvailabilityZone, PlacementAZ } from '../../../../helpers/dtos';
import { useAutoPlacement, useAvailabilityZones } from './placementsHelpers';
import { ReactComponent as WarningIcon } from './error_outline-24px.svg';
import { ReactComponent as CheckIcon } from './done-24px.svg';
import { ReactComponent as ErrorIcon } from './clear-24px.svg';
import { ControllerRenderProps, ValidationErrors } from '../../../../helpers/types';
import './PlacementsField.scss';

// add one more field to DTO types + allow gaps in node placements list
interface RegionInfo {
  parentRegionId: string;
  parentRegionName: string;
  parentRegionCode: string;
}
export type PlacementUI = (PlacementAZ & RegionInfo) | null;
export type AvailabilityZoneUI = AvailabilityZone & RegionInfo;

const getOptionLabel = (option: AvailabilityZoneUI): string =>
  `${option.parentRegionName}: ${option.name}`;
const getOptionValue = (option: AvailabilityZoneUI): string => option.uuid;

const FIELD_NAME = 'placements';
const MIN_PLACEMENTS_FOR_GEO_REDUNDANCY = 3;

export const PlacementsField: FC = () => {
  const { control, getValues, watch, errors } = useFormContext<CloudConfigFormValue>();
  const { operation } = useContext(WizardContext);

  // use watch() as component has to re-render itself on any of these values change
  const provider = watch('provider');
  const regionList = watch('regionList');
  const totalNodes = watch('totalNodes');
  const replicationFactor = watch('replicationFactor');
  const autoPlacement = watch('autoPlacement');

  // no need to re-render on placements change - thus use getValues()
  const currentPlacements = getValues(FIELD_NAME);

  const { zonesAll, zonesFiltered } = useAvailabilityZones(provider, regionList, currentPlacements);
  const { isLoading } = useAutoPlacement(
    FIELD_NAME,
    operation,
    regionList,
    totalNodes,
    replicationFactor,
    autoPlacement,
    zonesAll
  );

  const validate = (value: PlacementUI[]): boolean | string => {
    let message = '';
    const placementsNumber = _.compact(value).length;
    // watch('totalNodes') has old value as validation happens earlier than watchers update
    // thus explicitly ask for the latest value via getValues()
    const mostResentTotalNodes = getValues('totalNodes');

    // validate placements in manual mode only
    if (!autoPlacement) {
      if (!placementsNumber) {
        message = 'Please make sure to place zones';
      } else if (mostResentTotalNodes < replicationFactor) {
        message = 'Total of Individual Nodes should match or exceed the Replication Factor';
      }
    }

    return message || true;
  };

  return (
    <div className="availability-zones">
      <Controller
        control={control}
        name={FIELD_NAME}
        rules={{ validate }}
        render={({
          onChange,
          onBlur,
          value: placementsFormValue
        }: ControllerRenderProps<PlacementUI[]>) => (
          <>
            {_.isEmpty(placementsFormValue) && isLoading && (
              <div className="availability-zones__row">
                <YBLoadingCircleIcon size="medium" />
              </div>
            )}

            {!_.isEmpty(placementsFormValue) && (
              <>
                <Row className="availability-zones__row">
                  <Col xs={12}>
                    {(errors as ValidationErrors)[FIELD_NAME]?.message ? (
                      <div className="availability-zones__placements-label availability-zones__placements-label--error">
                        <ErrorIcon />
                        <I18n>{(errors as ValidationErrors)[FIELD_NAME]?.message}</I18n>
                      </div>
                    ) : _.compact(placementsFormValue).length <
                      MIN_PLACEMENTS_FOR_GEO_REDUNDANCY ? (
                      <div className="availability-zones__placements-label availability-zones__placements-label--warning">
                        <WarningIcon />
                        <I18n>
                          Primary node placement is not geo-redundant, configure nodes across at
                          least {MIN_PLACEMENTS_FOR_GEO_REDUNDANCY} zones to better withstand
                          availability zone failures
                        </I18n>
                      </div>
                    ) : (
                      <div className="availability-zones__placements-label availability-zones__placements-label--success">
                        <CheckIcon />
                        <I18n>
                          Primary node placement is geo-redundant, universe can survive at least 1
                          availability zone failure
                        </I18n>
                      </div>
                    )}
                  </Col>
                </Row>
                <Row className="availability-zones__row">
                  <Col sm={8}>
                    <I18n className="availability-zones__label">Name</I18n>
                  </Col>
                  <Col sm={4}>
                    <I18n className="availability-zones__label">Individual Nodes</I18n>
                  </Col>
                </Row>
              </>
            )}

            {placementsFormValue.map((placement, placementIndex) => (
              <Row key={placementIndex} className="availability-zones__row">
                <Col sm={8}>
                  <Select<AvailabilityZoneUI>
                    isSearchable={false}
                    isClearable={true}
                    isDisabled={autoPlacement}
                    getOptionLabel={getOptionLabel}
                    getOptionValue={getOptionValue}
                    value={zonesAll.find((item) => item.uuid === placement?.uuid) || null}
                    onBlur={onBlur}
                    onChange={(selection) => {
                      const newPlacement = _.cloneDeep(placementsFormValue);
                      if (selection) {
                        newPlacement[placementIndex] = {
                          uuid: (selection as AvailabilityZoneUI).uuid,
                          name: (selection as AvailabilityZoneUI).name,
                          parentRegionId: (selection as AvailabilityZoneUI).parentRegionId,
                          parentRegionName: (selection as AvailabilityZoneUI).parentRegionName,
                          parentRegionCode: (selection as AvailabilityZoneUI).parentRegionCode,
                          subnet: (selection as AvailabilityZoneUI).subnet,
                          // expected that configure call will put right "replicationFactor" before submitting actual changes to create/edit api
                          replicationFactor: newPlacement[placementIndex]?.replicationFactor || 1,
                          numNodesInAZ: newPlacement[placementIndex]?.numNodesInAZ || 1,
                          isAffinitized: true // by default all zones are "preferred"
                        };
                      } else {
                        newPlacement[placementIndex] = null;
                      }
                      onChange(newPlacement);
                    }}
                    options={zonesFiltered}
                  />
                </Col>
                <Col sm={4} className="availability-zones__nodes-col">
                  <Input
                    type="number"
                    min={1}
                    value={placement?.numNodesInAZ || ''}
                    className="availability-zones__number-input"
                    disabled={autoPlacement || _.isEmpty(placementsFormValue[placementIndex])}
                    onBlur={onBlur}
                    onChange={(event) => {
                      // strip all non-digit chars and forbid manual typing of zero or negative values
                      const newNodeCount = Number(event.target.value.replace(/\D/g, ''));
                      if (newNodeCount < 1) return;

                      const newPlacement = _.cloneDeep(placementsFormValue);
                      if (newPlacement[placementIndex]) {
                        // expected that configure call will put right "replicationFactor" before submitting actual changes to create/edit api
                        newPlacement[placementIndex]!.replicationFactor = 1;
                        newPlacement[placementIndex]!.numNodesInAZ = newNodeCount;
                      }

                      onChange(newPlacement);
                    }}
                  />
                  {isLoading && (
                    <div className="availability-zones__spinner">
                      <YBLoadingCircleIcon size="small" />
                    </div>
                  )}
                </Col>
              </Row>
            ))}

            {/*
            <pre>errors: {JSON.stringify(errors, null, 8)}</pre>
            <pre>value: {JSON.stringify(value, null, 8)}</pre>
            <pre>zonesAll (all zones for dropdown): {JSON.stringify(zonesAll, null, 8)}</pre>
            */}
          </>
        )}
      />
    </div>
  );
};
