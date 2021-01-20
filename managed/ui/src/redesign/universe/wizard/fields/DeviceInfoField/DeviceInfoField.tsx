import { useQuery } from 'react-query';
import { Col, Row } from 'react-bootstrap';
import { useUpdateEffect } from 'react-use';
import React, { FC, useContext } from 'react';
import { Controller, useFormContext } from 'react-hook-form';
import { Input } from '../../../../uikit/Input/Input';
import { InstanceConfigFormValue } from '../../steps/instance/InstanceConfig';
import { CloudType, DeviceInfo, InstanceType, StorageType } from '../../../../helpers/dtos';
import { I18n } from '../../../../uikit/I18n/I18n';
import { WizardContext } from '../../UniverseWizard';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { Select } from '../../../../uikit/Select/Select';
import { ControllerRenderProps } from '../../../../helpers/types';
import './DeviceInfoField.scss';

interface StorageTypeOption {
  value: StorageType;
  label: string;
}

const DEFAULT_STORAGE_TYPES = {
  [CloudType.aws]: StorageType.GP2,
  [CloudType.gcp]: StorageType.Scratch,
  [CloudType.azu]: StorageType.Premium_LRS
};

const DEFAULT_IOPS = 1000;

const AWS_STORAGE_TYPE_OPTIONS: StorageTypeOption[] = [
  { value: StorageType.IO1, label: 'IO1' },
  { value: StorageType.GP2, label: 'GP2' }
];

const GCP_STORAGE_TYPE_OPTIONS: StorageTypeOption[] = [
  { value: StorageType.Scratch, label: 'Local Scratch' },
  { value: StorageType.Persistent, label: 'Persistent' }
];

const AZURE_STORAGE_TYPE_OPTIONS: StorageTypeOption[] = [
  { value: StorageType.StandardSSD_LRS, label: 'Standard' },
  { value: StorageType.Premium_LRS, label: 'Premium' },
  { value: StorageType.UltraSSD_LRS, label: 'Ultra' }
];

const getStorageTypeOptions = (providerCode?: CloudType): StorageTypeOption[] => {
  switch (providerCode) {
    case CloudType.aws:
      return AWS_STORAGE_TYPE_OPTIONS;
    case CloudType.gcp:
      return GCP_STORAGE_TYPE_OPTIONS;
    case CloudType.azu:
      return AZURE_STORAGE_TYPE_OPTIONS;
    default:
      return [];
  }
};

const FIELD_NAME = 'deviceInfo';

const getDeviceInfoFromInstance = (instance: InstanceType): DeviceInfo | null => {
  if (instance.instanceTypeDetails.volumeDetailsList.length) {
    const { volumeDetailsList } = instance.instanceTypeDetails;

    return {
      numVolumes: volumeDetailsList.length,
      volumeSize: volumeDetailsList[0].volumeSizeGB,
      diskIops: null,
      storageClass: 'standard',
      storageType: DEFAULT_STORAGE_TYPES[instance.providerCode] ?? null,
      // see original at ClusterFields.js:492
      mountPoints:
        instance.providerCode === CloudType.onprem
          ? volumeDetailsList.flatMap((item) => item.mountPath).join(',')
          : null
    };
  } else {
    return null;
  }
};

export const DeviceInfoField: FC = () => {
  const { control, watch, setValue } = useFormContext<InstanceConfigFormValue>();
  const instanceType = watch('instanceType');
  const { formData } = useContext(WizardContext);
  const { data: instanceTypes } = useQuery(
    [QUERY_KEY.getInstanceTypes, formData.cloudConfig.provider?.uuid],
    api.getInstanceTypes,
    { enabled: !!formData.cloudConfig.provider?.uuid }
  );

  useUpdateEffect(() => {
    const instance = instanceTypes?.find((item) => item.instanceTypeCode === instanceType);

    if (instance) {
      setValue(FIELD_NAME, getDeviceInfoFromInstance(instance));
    } else {
      setValue(FIELD_NAME, null);
    }
  }, [instanceType]);

  const validate = () => true; // TODO

  return (
    <Controller
      control={control}
      name={FIELD_NAME}
      rules={{ validate }}
      render={({
        onChange,
        onBlur,
        value: deviceInfoFormValue
      }: ControllerRenderProps<DeviceInfo | null>) => (
        <>
          {deviceInfoFormValue && (
            <Row className="device-info-field__row">
              <Col sm={2}>
                <I18n className="device-info-field__label">Volume Info</I18n>
              </Col>
              <Col sm={10}>
                <div className="device-info-field">
                  <div className="device-info-field__inputs-block device-info-field__inputs-block--volume">
                    <Input
                      type="number"
                      // don't allow changing volumes number for onprem provider
                      disabled={formData.cloudConfig.provider?.code === CloudType.onprem}
                      min={1}
                      className="device-info-field__num-input"
                      onBlur={onBlur}
                      value={deviceInfoFormValue.numVolumes}
                      onChange={(event) => {
                        const numVolumes = Number(event.target.value.replace(/\D/g, ''));
                        if (numVolumes > 0) onChange({ ...deviceInfoFormValue, numVolumes });
                      }}
                    />
                    <span className="device-info-field__x-symbol" />
                    <Input
                      type="number"
                      // don't allow changing volume size for GCP or onprem providers
                      disabled={
                        formData.cloudConfig.provider?.code === CloudType.gcp ||
                        formData.cloudConfig.provider?.code === CloudType.onprem
                      }
                      min={1}
                      className="device-info-field__num-input"
                      onBlur={onBlur}
                      value={deviceInfoFormValue.volumeSize}
                      onChange={(event) => {
                        const volumeSize = Number(event.target.value.replace(/\D/g, ''));
                        if (volumeSize > 0) onChange({ ...deviceInfoFormValue, volumeSize });
                      }}
                    />
                  </div>

                  {deviceInfoFormValue.storageType && (
                    <div className="device-info-field__inputs-block device-info-field__inputs-block--storage-type">
                      <I18n className="device-info-field__label device-info-field__label--margin-right">
                        {formData.cloudConfig.provider?.code === CloudType.aws
                          ? 'EBS Type'
                          : 'Storage Type (SSD)'}
                      </I18n>
                      <Select<StorageTypeOption>
                        isSearchable={false}
                        isClearable={false}
                        isDisabled={false}
                        className="device-info-field__storage-type"
                        value={getStorageTypeOptions(formData.cloudConfig.provider?.code).find(
                          (item) => item.value === deviceInfoFormValue.storageType
                        )}
                        onBlur={onBlur}
                        onChange={(selection) => {
                          const storageType = (selection as StorageTypeOption).value;
                          if (storageType === StorageType.IO1) {
                            onChange({
                              ...deviceInfoFormValue,
                              diskIops: DEFAULT_IOPS,
                              storageType
                            });
                          } else {
                            onChange({ ...deviceInfoFormValue, diskIops: null, storageType });
                          }
                        }}
                        options={getStorageTypeOptions(formData.cloudConfig.provider?.code)}
                      />

                      {deviceInfoFormValue.storageType === StorageType.IO1 && (
                        <>
                          <I18n className="device-info-field__label device-info-field__label--margin-right">Provisioned IOPS</I18n>
                          <Input
                            type="number"
                            min={1}
                            className="device-info-field__num-input"
                            onBlur={onBlur}
                            value={deviceInfoFormValue.diskIops || DEFAULT_IOPS}
                            onChange={(event) => {
                              const diskIops = Number(event.target.value.replace(/\D/g, ''));
                              onChange({ ...deviceInfoFormValue, diskIops });
                            }}
                          />
                        </>
                      )}
                    </div>
                  )}
                </div>
              </Col>
            </Row>
          )}
        </>
      )}
    />
  );
};
