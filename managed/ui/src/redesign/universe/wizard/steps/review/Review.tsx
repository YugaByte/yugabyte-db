import _ from 'lodash';
import { useQuery } from 'react-query';
import React, { Dispatch, FC, ReactNode, useContext } from 'react';
import { Col, Grid, Row } from 'react-bootstrap';
import pluralize from 'pluralize';
import { I18n } from '../../../../uikit/I18n/I18n';
import { WizardAction, WizardContext } from '../../UniverseWizard';
import { Button } from '../../../../uikit/Button/Button';
import { WizardStep, WizardStepper } from '../../compounds/WizardStepper/WizardStepper';
import { Summary } from '../../compounds/Summary/Summary';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { sortVersionStrings } from '../../../../../utils/ObjectUtils';
import { YBLoading } from '../../../../../components/common/indicators/index';
import { ReviewSection } from '../../compounds/ExpandableSection/ReviewSection';
import { useLaunchUniverse } from './reviewHelpers';
import { CloudType } from '../../../../helpers/dtos';
import { cloudProviders } from '../instance/InstanceConfig';
import '../StepWrapper.scss';
import './Review.scss';

// required fields that have to be set, but there's no dedicated UI for them
export interface HiddenConfig {
  accessKeyCode: string | null;
  enableYSQL: boolean;
  userAZSelected: boolean;
  useTimeSync: boolean;
  installNodeExporter: boolean; // TODO: clarify, maybe it's not hidden field
}

interface ReviewProps {
  dispatch: Dispatch<WizardAction>;
}

const renderValue = (value: unknown): ReactNode => {
  if (_.isBoolean(value)) return value ? <I18n>Yes</I18n> : <I18n>No</I18n>;

  if (_.isNumber(value) || (_.isString(value) && value !== '')) return value;

  // take care of empty string, null, undefined, [] and {}
  if (_.isEmpty(value)) return <I18n>(not set)</I18n>;

  if (_.isObject(value)) {
    return Object.keys(value).map((key) => (
      <div key={key}>
        {key}: {value[key]}
      </div>
    ));
  }

  // safety fallback, should never be here
  return JSON.stringify(value);
};

export const Review: FC<ReviewProps> = ({ dispatch }) => {
  const { formData } = useContext(WizardContext);
  const { isLaunchingUniverse, launchUniverse } = useLaunchUniverse();

  // populate DB Version field if it's not set (i.e. user jumped to the Review step skipping DB Config)
  const { isLoading: isDBVersionLoading } = useQuery(QUERY_KEY.getDBVersions, api.getDBVersions, {
    enabled: !formData.dbConfig.ybSoftwareVersion,
    onSuccess: (data) => {
      // pre-select first available db version
      const sorted: string[] = sortVersionStrings(data);
      dispatch({
        type: 'update_form_data',
        payload: {
          dbConfig: {
            ...formData.dbConfig,
            ybSoftwareVersion: sorted[0]
          }
        }
      });
    }
  });

  // get access key for provider as we don't have such field in UI
  const { isLoading: isAccessKeyLoading } = useQuery(
    [QUERY_KEY.getAccessKeys, formData.cloudConfig.provider?.uuid],
    api.getAccessKeys,
    {
      // prevent query from running for edit mode or when there's no provider set
      enabled: !formData.hiddenConfig.accessKeyCode && !!formData.cloudConfig.provider?.uuid,
      onSuccess: (data) => {
        // currently there's single access key per provider, so just take first item from an array
        if (!_.isEmpty(data)) {
          dispatch({
            type: 'update_form_data',
            payload: {
              hiddenConfig: {
                ...formData.hiddenConfig,
                accessKeyCode: data[0].idKey.keyCode
              }
            }
          });
        }
      }
    }
  );

  const { isLoading: isProvidersLoading, data: providersList } = useQuery(
    QUERY_KEY.getProvidersList,
    api.getProvidersList
  );

  const { isLoading: isCertificatesLoading, data: certificatesList } = useQuery(
    QUERY_KEY.getCertificates,
    api.getCertificates
  );

  const { isLoading: isKmsConfigsLoading, data: kmsConfigs } = useQuery(
    QUERY_KEY.getKMSConfigs,
    api.getKMSConfigs
  );

  const cancel = () => dispatch({ type: 'exit_wizard', payload: true });
  const jumpToStep = (nextStep: WizardStep) => dispatch({ type: 'change_step', payload: nextStep });

  const isLoading =
    isDBVersionLoading ||
    isAccessKeyLoading ||
    isProvidersLoading ||
    isCertificatesLoading ||
    isKmsConfigsLoading;

  return (
    <div className="wizard-step-wrapper">
      <div className="wizard-step-wrapper__stepper">
        <WizardStepper activeStep={WizardStep.Review} clickableTabs={true} onChange={jumpToStep} />
      </div>
      <div className="wizard-step-wrapper__container">
        <div className="wizard-step-wrapper__col-form">
          <Grid fluid>
            <div className="review-step">
              {isLoading ? (
                <YBLoading />
              ) : (
                <>
                  <ReviewSection
                    title={<I18n>Cloud Config</I18n>}
                    expanded
                    onEditClick={() => jumpToStep(WizardStep.Cloud)}
                  >
                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Universe Name</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.cloudConfig.universeName)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Provider</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(
                          providersList?.find(
                            (item) => item.uuid === formData.cloudConfig.provider?.uuid
                          )?.name
                        )}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Regions and Placements</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {_.compact(formData.cloudConfig.placements).map((item) => (
                          <div key={item.uuid}>
                            {item.parentRegionName}: {item.name}
                            &nbsp;—&nbsp;
                            {pluralize('node', item.numNodesInAZ, true)}
                          </div>
                        ))}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Replication Factor</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.cloudConfig.replicationFactor)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Total Nodes</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.cloudConfig.totalNodes)}
                      </Col>
                    </Row>
                  </ReviewSection>

                  <ReviewSection
                    title={<I18n>Instance Config</I18n>}
                    expanded
                    onEditClick={() => jumpToStep(WizardStep.Instance)}
                  >
                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Instance Type</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.instanceConfig.instanceType)}
                      </Col>
                    </Row>
                    {formData.instanceConfig.deviceInfo && (
                      <Row className="review-step__row">
                        <Col xs={3} className="review-step__field-title">
                          <I18n>Volume Info</I18n>
                        </Col>
                        <Col xs={9} className="review-step__field-value">
                          {formData.instanceConfig.deviceInfo?.numVolumes}
                          &nbsp;x&nbsp;
                          {formData.instanceConfig.deviceInfo?.volumeSize}
                          &nbsp;
                          {formData.instanceConfig.deviceInfo?.storageType}
                          &nbsp;
                          {formData.instanceConfig.deviceInfo?.diskIops &&
                            formData.instanceConfig.deviceInfo?.diskIops + ' IOPS'}
                        </Col>
                      </Row>
                    )}
                    {formData.cloudConfig.provider?.code === CloudType.aws && (
                      <Row className="review-step__row">
                        <Col xs={3} className="review-step__field-title">
                          <I18n>Instance Tags</I18n>
                        </Col>
                        <Col xs={9} className="review-step__field-value">
                          {renderValue(formData.instanceConfig.instanceTags)}
                        </Col>
                      </Row>
                    )}
                    {cloudProviders.has(formData.cloudConfig.provider?.code as CloudType) && (
                      <Row className="review-step__row">
                        <Col xs={3} className="review-step__field-title">
                          <I18n>Assign Public IP</I18n>
                        </Col>
                        <Col xs={9} className="review-step__field-value">
                          {renderValue(formData.instanceConfig.assignPublicIP)}
                        </Col>
                      </Row>
                    )}
                    {formData.cloudConfig.provider?.code === CloudType.aws && (
                      <Row className="review-step__row">
                        <Col xs={3} className="review-step__field-title">
                          <I18n>Instance Profile ARN</I18n>
                        </Col>
                        <Col xs={9} className="review-step__field-value">
                          {renderValue(formData.instanceConfig.awsArnString)}
                        </Col>
                      </Row>
                    )}
                  </ReviewSection>

                  <ReviewSection
                    title={<I18n>DB Config</I18n>}
                    expanded
                    onEditClick={() => jumpToStep(WizardStep.Db)}
                  >
                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>DB Version</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.dbConfig.ybSoftwareVersion)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Preferred Leaders</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {formData.dbConfig.preferredLeaders.map((item) => (
                          <div key={item.uuid}>
                            {item.parentRegionName}: {item.name}
                          </div>
                        ))}
                        {_.isEmpty(formData.dbConfig.preferredLeaders) && renderValue(null)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Communication Ports</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.dbConfig.communicationPorts)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>YB-Master Config Flags</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.dbConfig.masterGFlags)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>YB-TServer Config Flags</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.dbConfig.tserverGFlags)}
                      </Col>
                    </Row>
                  </ReviewSection>

                  <ReviewSection
                    title={<I18n>Security Config</I18n>}
                    expanded
                    onEditClick={() => jumpToStep(WizardStep.Security)}
                  >
                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Enable Authentication</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.securityConfig.enableAuthentication)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Enable Node-to-Node TLS</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.securityConfig.enableNodeToNodeEncrypt)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Enable Client-to-Node TLS</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.securityConfig.enableClientToNodeEncrypt)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Root Certificate</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(
                          certificatesList?.find(
                            (item) => item.uuid === formData.securityConfig.rootCA
                          )?.label
                        )}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>Enable Encryption at-rest</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(formData.securityConfig.enableEncryptionAtRest)}
                      </Col>
                    </Row>

                    <Row className="review-step__row">
                      <Col xs={3} className="review-step__field-title">
                        <I18n>KMS Config</I18n>
                      </Col>
                      <Col xs={9} className="review-step__field-value">
                        {renderValue(
                          kmsConfigs?.find(
                            (item) => item.metadata.configUUID === formData.securityConfig.kmsConfig
                          )?.metadata.name
                        )}
                      </Col>
                    </Row>
                  </ReviewSection>
                </>
              )}
            </div>
          </Grid>

          <div className="review-step__footer-row">
            <Button className="review-step__footer-btn" onClick={cancel}>
              <I18n>Cancel</I18n>
            </Button>
            <Button
              chevronLeft
              className="review-step__footer-btn"
              onClick={() => jumpToStep(WizardStep.Security)}
            >
              <I18n>Previous</I18n>
            </Button>
            {!isLoading && (
              <Button
                isCTA
                className="review-step__footer-btn"
                onClick={launchUniverse}
                disabled={isLaunchingUniverse}
              >
                <I18n>Launch Universe</I18n>
              </Button>
            )}
          </div>
        </div>
        <div className="wizard-step-wrapper__col-summary">
          <Summary formData={formData} />
        </div>
      </div>
    </div>
  );
};
