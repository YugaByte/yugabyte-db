import _ from 'lodash';
import * as Yup from 'yup';
import React, { useState } from 'react';
import { Field, Form, Formik } from 'formik';
import { Row, Col } from 'react-bootstrap';
import { YBButton, YBFormInput } from '../../../common/forms/fields';
import { isEnabled } from '../../../../utils/LayoutUtils';
import { AzureRegions } from './AzureRegions';

const initialValues = {
  providerName: '', // not a part of config payload
  networkSetup: 'existing_vpc', // not a part of config payload
  AZURE_CLIENT_ID: '',
  AZURE_CLIENT_SECRET: '',
  AZURE_TENANT_ID: '',
  AZURE_SUBSCRIPTION_ID: '',
  AZURE_RG: ''
};

const validationSchema = Yup.object().shape({
  providerName: Yup.string().required('Provider Name is a required field'),
  AZURE_CLIENT_ID: Yup.string().required('Azure Client ID is a required field'),
  AZURE_CLIENT_SECRET: Yup.string().required('Azure Client Secret is a required field'),
  AZURE_TENANT_ID: Yup.string().required('Azure Tenant ID is a required field'),
  AZURE_SUBSCRIPTION_ID: Yup.string().required('Azure Subscription ID is a required field'),
  AZURE_RG: Yup.string().required('Azure Resource Group is a required field')
});

const convertFormDataToPayload = (formData) => {
  const perRegionMetadata = {};

  formData.forEach(regionItem => {
    const azToSubnetIds = {};
    regionItem.azToSubnetIds.forEach(zoneItem => {
      if (zoneItem.zone?.value) {
        azToSubnetIds[zoneItem.zone?.value] = zoneItem.subnet;
      }
    });

    perRegionMetadata[regionItem.region.value] = {
      vpcId: regionItem.vpcId,
      customSecurityGroupId: regionItem.customSecurityGroupId,
      customImageId: regionItem.customImageId,
      azToSubnetIds
    };
  });

  return { perRegionMetadata };
};

export const AzureProviderInitView = ({ featureFlags, createAzureProvider }) => {
  const [regionsFormData, setRegionsFormData] = useState([]);
  const isAzureEnabled = isEnabled(featureFlags, 'universes.providers.azure', false);

  const createProviderConfig = (values) => {
    const config = _.omit(values, 'providerName', 'networkSetup');
    const regions = convertFormDataToPayload(regionsFormData);
    createAzureProvider(values.providerName, config, regions);
  };

  return (
    <div className="provider-config-container">
      {isAzureEnabled ? (
        <Formik
          initialValues={initialValues}
          validationSchema={validationSchema}
          onSubmit={createProviderConfig}
        >
          {({ isValid }) => (
            <Form>
              <Row>
                <Col lg={10}>
                  <Row className="config-provider-row">
                    <Col lg={3}>
                      <div className="form-item-custom-label">Provider Name</div>
                    </Col>
                    <Col lg={7}>
                      <Field name="providerName" placeholder="Provider Name" component={YBFormInput} />
                    </Col>
                  </Row>
                  <Row className="config-provider-row">
                    <Col lg={3}>
                      <div className="form-item-custom-label">Client ID</div>
                    </Col>
                    <Col lg={7}>
                      <Field name="AZURE_CLIENT_ID" placeholder="Client ID" component={YBFormInput} />
                    </Col>
                  </Row>
                  <Row className="config-provider-row">
                    <Col lg={3}>
                      <div className="form-item-custom-label">Client Secret</div>
                    </Col>
                    <Col lg={7}>
                      <Field name="AZURE_CLIENT_SECRET" placeholder="Client Secret" component={YBFormInput} />
                    </Col>
                  </Row>
                  <Row className="config-provider-row">
                    <Col lg={3}>
                      <div className="form-item-custom-label">Tenant ID</div>
                    </Col>
                    <Col lg={7}>
                      <Field name="AZURE_TENANT_ID" placeholder="Tenant ID" component={YBFormInput} />
                    </Col>
                  </Row>
                  <Row className="config-provider-row">
                    <Col lg={3}>
                      <div className="form-item-custom-label">Subscription ID</div>
                    </Col>
                    <Col lg={7}>
                      <Field name="AZURE_SUBSCRIPTION_ID" placeholder="Subscription ID" component={YBFormInput} />
                    </Col>
                  </Row>
                  <Row className="config-provider-row">
                    <Col lg={3}>
                      <div className="form-item-custom-label">Resource Group</div>
                    </Col>
                    <Col lg={7}>
                      <Field name="AZURE_RG" placeholder="Resource Group" component={YBFormInput} />
                    </Col>
                  </Row>

                  <Row className="config-provider-row">
                    <Col lg={3}>
                      <div className="form-item-custom-label">Virtual Network Setup</div>
                    </Col>
                    <Col lg={7}>
                      <Field name="networkSetup">
                        {({ field }) => (
                          <select
                            name="networkSetup"
                            className="form-control"
                            value={field.value}
                            onChange={field.onChange}
                          >
                            <option key={1} value="new_vpc" disabled={true}>Create a new Virtual Network</option>
                            <option key={2} value="existing_vpc">Specify an existing Virtual Network</option>
                          </select>
                        )}
                      </Field>
                    </Col>
                  </Row>
                  <Row className="config-provider-row">
                    <Col lg={10}>
                      <AzureRegions regions={regionsFormData} onChange={value => setRegionsFormData(value)} />
                    </Col>
                  </Row>

                </Col>
              </Row>
              <div className="form-action-button-container">
                <YBButton btnType="submit" btnClass="btn btn-default save-btn" btnText="Save" disabled={!isValid} />
              </div>
            </Form>
          )}
        </Formik>
      ) : (
        <div className="provider-config-container">
          Stay tuned for our Microsoft Azure integration.
        </div>
      )}
    </div>
  );
};

