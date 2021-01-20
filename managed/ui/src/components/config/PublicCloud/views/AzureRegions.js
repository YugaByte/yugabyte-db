import _ from 'lodash';
import * as Yup from 'yup';
import { Field, Formik } from 'formik';
import { Col, Row } from 'react-bootstrap';
import React, { useEffect, useState } from 'react';
import { YBFormInput, YBFormSelect, YBModal } from '../../../common/forms/fields';

const AZURE_REGIONS = [
  {
    region: 'westus2',
    name: 'US West 2 (Washington)',
    zones: ['westus2-1', 'westus2-2', 'westus2-3']
  },
  {
    region: 'centralus',
    name: 'US Central (Iowa)',
    zones: ['centralus-1', 'centralus-2', 'centralus-3']
  },
  {
    region: 'eastus',
    name: 'US East (Virginia)',
    zones: ['eastus-1', 'eastus-2', 'eastus-3']
  },
  {
    region: 'eastus2',
    name: 'US East 2 (Virginia)',
    zones: ['eastus2-1', 'eastus2-2', 'eastus2-3']
  },
  {
    region: 'westeurope',
    name: 'West Europe (Netherlands)',
    zones: ['westeurope-1', 'westeurope-2', 'westeurope-3']
  },
  {
    region: 'northeurope',
    name: 'North Europe (Ireland)',
    zones: ['northeurope-1', 'northeurope-2', 'northeurope-3']
  },
  {
    region: 'japaneast',
    name: 'Japan East (Tokyo)',
    zones: ['japaneast-1', 'japaneast-2', 'japaneast-3']
  },
  {
    region: 'southeastasia',
    name: 'Southeast Asia (Singapore)',
    zones: ['southeastasia-1', 'southeastasia-2', 'southeastasia-3']
  },
  {
    region: 'australiaeast',
    name: 'Australia East (New South Wales)',
    zones: ['australiaeast-1', 'australiaeast-2', 'australiaeast-3']
  },
  {
    region: 'uksouth',
    name: 'UK South (London)',
    zones: ['uksouth-1', 'uksouth-2', 'uksouth-3']
  },
  {
    region: 'francecentral',
    name: 'France Central (Paris)',
    zones: ['francecentral-1', 'francecentral-2', 'francecentral-3']
  }
];

const zonesMap = {};
AZURE_REGIONS.forEach((item) => {
  zonesMap[item.region] = item.zones.map((zone) => ({ value: zone, label: zone }));
});

const initialRegionForm = {
  region: '',
  vpcId: '',
  customSecurityGroupId: '',
  customImageId: '',
  azToSubnetIds: [
    { zone: '', subnet: '' },
    { zone: '', subnet: '' },
    { zone: '', subnet: '' }
  ]
};

const validationSchema = Yup.object().shape({
  region: Yup.object().required('Region is a required field'),
  vpcId: Yup.string().required('Virtual Network Name is a required field'),
  azToSubnetIds: Yup.lazy((value) => {
    const isAnyZoneSelected = value.some((item) => !_.isEmpty(item.zone?.value));
    if (isAnyZoneSelected) {
      return Yup.array().of(
        Yup.object().shape({
          subnet: Yup.string().when('zone', {
            is: (value) => !_.isEmpty(value),
            then: Yup.string().required('Subnet is a required field')
          })
        })
      );
    } else {
      return Yup.array().test(
        'non-empty-test',
        'At least one zone has to be selected',
        () => false
      );
    }
  })
});

export const AzureRegions = ({ regions, onChange }) => {
  const [isModalVisible, setModalVisibility] = useState(false);
  const [availableRegions, setAvailableRegions] = useState([]);
  const [currentRegion, setCurrentRegion] = useState(); // region that is currently edited in modal

  // remove already selected regions from the list and convert them to rendering format
  useEffect(() => {
    const selectedRegions = _.map(regions, 'region.value');
    const result = AZURE_REGIONS
      .filter(item => !selectedRegions.includes(item.region))
      .map((region) => ({ value: region.region, label: region.name }));

    // when editing existing region - make sure to keep it in dropdown options
    if (currentRegion) {
      result.push({ value: currentRegion.region.value, label: currentRegion.region.label });
    }

    setAvailableRegions(result);
  }, [regions, currentRegion]);

  const removeRegion = (item) => onChange(_.without(regions, item));

  const editRegion = (item) => {
    setCurrentRegion(item);
    setModalVisibility(true);
  };

  return (
    <div>
      <h4 className="regions-form-title">Regions</h4>

      {!_.isEmpty(regions) && (
        <ul className="config-region-list">
          <li className="header-row">
            <div>Name</div>
            <div>Virtual Network</div>
            <div>Security Group</div>
            <div>Zones</div>
            <div />
          </li>
          {regions.map((item) => (
            <li key={item.region.value} onClick={() => editRegion(item)}>
              <div>
                <strong>{item.region.label}</strong>
              </div>
              <div>{item.vpcId}</div>
              <div>{item.customSecurityGroupId}</div>
              <div>{item.azToSubnetIds.filter((item) => !_.isEmpty(item.zone.value)).length}</div>
              <div>
                <button
                  type="button"
                  className="delete-provider"
                  onClick={(event) => {
                    event.stopPropagation();
                    removeRegion(item);
                  }}
                >
                  <i className="fa fa-times fa-fw delete-row-btn"></i>
                </button>
              </div>
            </li>
          ))}
        </ul>
      )}
      <button
        type="button"
        className="btn btn-default btn-add-region"
        onClick={() => editRegion(null)}
      >
        <div className="btn-icon">
          <i className="fa fa-plus"></i>
        </div>{' '}
        Add Region
      </button>

      {isModalVisible && (
        <Formik
          initialValues={currentRegion || initialRegionForm}
          validationSchema={validationSchema}
        >
          {({ values, errors, isValid, setFieldValue }) => (
            <YBModal
              formName="azureRegionsConfig"
              visible={true}
              submitLabel={currentRegion ? 'Edit Region' : 'Add Region'}
              showCancelButton={true}
              disableSubmit={!isValid}
              title="Specify Region Info"
              onHide={() => setModalVisibility(false)}
              onFormSubmit={() => {
                let updatedRegions;
                if (currentRegion) {
                  const i = regions.findIndex((item) => item === currentRegion);
                  regions[i] = values;
                  updatedRegions = regions;
                } else {
                  updatedRegions = [...regions, values];
                }

                onChange(updatedRegions);
                setModalVisibility(false);
              }}
            >
              <div>
                <div className="form-item-custom-label">Region</div>
                <Field
                  name="region"
                  component={YBFormSelect}
                  options={availableRegions}
                  onChange={({ field }, value) => {
                    // reset zones on region change and then update region itself
                    setFieldValue('azToSubnetIds', initialRegionForm.azToSubnetIds);
                    setFieldValue('region', value);
                  }}
                />
              </div>
              <div>
                <div className="form-item-custom-label">Virtual Network Name</div>
                <Field name="vpcId" component={YBFormInput} />
              </div>
              <div>
                <div className="form-item-custom-label">Security Group Name (optional)</div>
                <Field name="customSecurityGroupId" component={YBFormInput} />
              </div>
              <div>
                <div className="form-item-custom-label">Marketplace Image URN (optional)</div>
                <Field name="customImageId" component={YBFormInput} />
              </div>
              <div className="divider" />
              <h5>AZ Mapping</h5>
              {values.azToSubnetIds.map((item, index) => (
                <Row key={index}>
                  <Col lg={6}>
                    <Field
                      name={`azToSubnetIds[${index}].zone`}
                      component={YBFormSelect}
                      isClearable={true}
                      options={
                        // skip already selected zones from dropdown options
                        (zonesMap[values.region.value] || []).filter((item) => {
                          const selectedZones = _.compact(
                            _.map(values.azToSubnetIds, 'zone.value')
                          );
                          return !selectedZones.includes(item.value);
                        })
                      }
                    />
                  </Col>
                  <Col lg={6}>
                    <Field
                      name={`azToSubnetIds[${index}].subnet`}
                      component={YBFormInput}
                      placeholder="Subnet"
                    />
                  </Col>
                </Row>
              ))}
              {_.isString(errors.azToSubnetIds) && (
                <div className="has-error">
                  <span className="help-block">{errors.azToSubnetIds}</span>
                </div>
              )}
            </YBModal>
          )}
        </Formik>
      )}
    </div>
  );
};
