import React, { Dispatch, FC, useContext } from 'react';
import { FormProvider, useForm } from 'react-hook-form';
import { Col, Grid, Row } from 'react-bootstrap';
import { I18n } from '../../../../uikit/I18n/I18n';
import { Button } from '../../../../uikit/Button/Button';
import { Summary } from '../../compounds/Summary/Summary';
import { WizardStep, WizardStepper } from '../../compounds/WizardStepper/WizardStepper';
import { WizardAction, WizardContext } from '../../UniverseWizard';
import { EnableAuthenticationField } from '../../fields/EnableAuthenticationField/EnableAuthenticationField';
import { EnableNodeToNodeTLSField } from '../../fields/EnableNodeToNodeTLSField/EnableNodeToNodeTLSField';
import { EnableClientToNodeTLSField } from '../../fields/EnableClientToNodeTLSField/EnableClientToNodeTLSField';
import { EnableEncryptionAtRestField } from '../../fields/EnableEncryptionAtRestField/EnableEncryptionAtRestField';
import { RootCertificateField } from '../../fields/RootCertificateField/RootCertificateField';
import { KMSConfigField } from '../../fields/KMSConfigField/KMSConfigField';
import '../StepWrapper.scss';
import './SecurityConfig.scss';

export interface SecurityConfigFormValue {
  enableAuthentication: boolean;
  enableNodeToNodeEncrypt: boolean;
  enableClientToNodeEncrypt: boolean;
  enableEncryptionAtRest: boolean;
  rootCA: string | null; // root certificate ID
  kmsConfig: string | null; // key management service config ID
}

interface SecurityConfigProps {
  dispatch: Dispatch<WizardAction>;
}

export const SecurityConfig: FC<SecurityConfigProps> = ({ dispatch }) => {
  const { isEditMode, formData } = useContext(WizardContext);
  const formMethods = useForm<SecurityConfigFormValue>({
    mode: 'onChange',
    defaultValues: formData.securityConfig
  });

  const cancel = () => dispatch({ type: 'exit_wizard', payload: true });

  const submit = async (nextStep: WizardStep): Promise<void> => {
    const isValid = await formMethods.trigger();
    if (isValid) {
      const formValues: SecurityConfigFormValue = formMethods.getValues();
      dispatch({ type: 'update_form_data', payload: { securityConfig: formValues } });
      dispatch({ type: 'change_step', payload: nextStep });
    }
  };

  return (
    <div className="wizard-step-wrapper">
      <div className="wizard-step-wrapper__stepper">
        <WizardStepper activeStep={WizardStep.Security} clickableTabs onChange={submit} />
      </div>
      <div className="wizard-step-wrapper__container">
        <div className="wizard-step-wrapper__col-form">
          <FormProvider {...formMethods}>
            <Grid fluid>
              <div className="security-config">
                <Row>
                  <Col xs={12}>
                    <div className="security-config__title">
                      <I18n>Authentication</I18n>
                    </div>
                  </Col>
                </Row>

                <Row className="security-config__row">
                  <Col sm={2}>
                    <I18n className="security-config__label">Enable Authentication</I18n>
                  </Col>
                  <Col sm={6}>
                    <EnableAuthenticationField disabled={isEditMode} />
                  </Col>
                </Row>

                <Row className="security-config__row">
                  <Col xs={12}>
                    <div className="security-config__title">
                      <I18n>In-Flight TLS Encryption</I18n>
                    </div>
                  </Col>
                </Row>

                <Row className="security-config__row">
                  <Col sm={12} className="security-config__label-line">
                    <I18n className="security-config__label">Node-to-Node TLS</I18n>
                    <div className="security-config__line" />
                  </Col>
                </Row>
                <Row className="security-config__row">
                  <Col sm={12} className="security-config__padded">
                    <EnableNodeToNodeTLSField disabled={isEditMode} />
                  </Col>
                </Row>

                <Row className="security-config__row">
                  <Col sm={12} className="security-config__label-line">
                    <I18n className="security-config__label">Client-to-Node TLS</I18n>
                    <div className="security-config__line" />
                  </Col>
                </Row>
                <Row className="security-config__row">
                  <Col sm={12} className="security-config__padded">
                    <EnableClientToNodeTLSField disabled={isEditMode} />
                  </Col>
                </Row>

                <Row className="security-config__row">
                  <Col sm={2}>
                    <I18n className="security-config__label">Root Certificate</I18n>
                  </Col>
                  <Col sm={6}>
                    <RootCertificateField
                      disabled={
                        isEditMode ||
                        !(
                          formMethods.watch('enableNodeToNodeEncrypt') ||
                          formMethods.watch('enableClientToNodeEncrypt')
                        )
                      }
                    />
                  </Col>
                </Row>

                <Row className="security-config__row">
                  <Col xs={12}>
                    <div className="security-config__title">
                      <I18n>Encryption at-rest</I18n>
                    </div>
                  </Col>
                </Row>
                <Row className="security-config__row">
                  <Col sm={12} className="security-config__padded">
                    <EnableEncryptionAtRestField disabled={isEditMode} />
                  </Col>
                </Row>
                <Row className="security-config__row">
                  <Col sm={2} className="security-config__padded">
                    <I18n className="security-config__label">Key Management Service Config</I18n>
                  </Col>
                  <Col sm={6}>
                    <KMSConfigField
                      disabled={isEditMode || !formMethods.watch('enableEncryptionAtRest')}
                    />
                  </Col>
                </Row>
              </div>
            </Grid>

            <div className="security-config__footer-row">
              <Button className="security-config__footer-btn" onClick={cancel}>
                <I18n>Cancel</I18n>
              </Button>
              <Button
                chevronLeft
                className="security-config__footer-btn"
                onClick={() => submit(WizardStep.Db)}
              >
                <I18n>Previous</I18n>
              </Button>
              <Button
                isCTA
                className="security-config__footer-btn"
                onClick={() => submit(WizardStep.Review)}
              >
                <I18n>Review and Launch</I18n>
              </Button>
            </div>

            <Row>
              <Col xs={4}>
                <pre>Form Values: {JSON.stringify(formMethods.watch(), null, 8)}</pre>
              </Col>
              <Col xs={4}>
                <pre>Form State: {JSON.stringify(formMethods.formState, null, 8)}</pre>
              </Col>
              <Col xs={4}>
                <pre>Errors: {JSON.stringify(formMethods.errors, null, 8)}</pre>
              </Col>
            </Row>

          </FormProvider>
        </div>
        <div className="wizard-step-wrapper__col-summary">
          <Summary formData={formData} />
        </div>
      </div>
    </div>
  );
};
