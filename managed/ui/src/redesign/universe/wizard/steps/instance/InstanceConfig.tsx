import React, { Dispatch, FC, useContext } from 'react';
import { Col, Grid, Row } from 'react-bootstrap';
import { FormProvider, useForm } from 'react-hook-form';
import { I18n } from '../../../../uikit/I18n/I18n';
import { Button } from '../../../../uikit/Button/Button';
import { WizardAction, WizardContext } from '../../UniverseWizard';
import { InstanceTypeField } from '../../fields/InstanceTypeField/InstanceTypeField';
import { CloudType, DeviceInfo, FlagsObject } from '../../../../helpers/dtos';
import { DeviceInfoField } from '../../fields/DeviceInfoField/DeviceInfoField';
import { InstanceTagsField } from '../../fields/InstanceTagsField/InstanceTagsField';
import { AdvancedSettings } from '../../compounds/ExpandableSection/AdvancedSettings';
import { AssignPublicIPField } from '../../fields/AssignPublicIPField/AssignPublicIPField';
import { ProfileARNField } from '../../fields/ProfileARNField/ProfileARNField';
import { WizardStep, WizardStepper } from '../../compounds/WizardStepper/WizardStepper';
import { Summary } from '../../compounds/Summary/Summary';
import '../StepWrapper.scss';
import './InstanceConfig.scss';

export interface InstanceConfigFormValue {
  instanceType: string | null;
  deviceInfo: DeviceInfo | null;
  instanceTags: FlagsObject;
  assignPublicIP: boolean;
  awsArnString: string | null;
}

interface InstanceConfigProps {
  dispatch: Dispatch<WizardAction>;
}

export const cloudProviders = new Set([CloudType.aws, CloudType.gcp, CloudType.azu]);

export const InstanceConfig: FC<InstanceConfigProps> = ({ dispatch }) => {
  const { isEditMode, formData } = useContext(WizardContext);
  const formMethods = useForm<InstanceConfigFormValue>({
    mode: 'onChange',
    defaultValues: formData.instanceConfig
  });

  const cancel = () => dispatch({ type: 'exit_wizard', payload: true });

  // "Review and Launch" and "Next" are both form submit buttons but with different navigation destination
  // so manually trigger form validation and dispatch "change_step" action with proper destination payload
  const submit = async (nextStep: WizardStep): Promise<void> => {
    const isValid = await formMethods.trigger();
    if (isValid) {
      const formValues: InstanceConfigFormValue = formMethods.getValues();
      dispatch({ type: 'update_form_data', payload: { instanceConfig: formValues } });
      dispatch({ type: 'change_step', payload: nextStep });
    }
  };

  return (
    <div className="wizard-step-wrapper">
      <div className="wizard-step-wrapper__stepper">
        <WizardStepper activeStep={WizardStep.Instance} clickableTabs onChange={submit} />
      </div>
      <div className="wizard-step-wrapper__container">
        <div className="wizard-step-wrapper__col-form">
          <FormProvider {...formMethods}>
            <Grid fluid>
              <div className="instance-config">
                <Row>
                  <Col xs={12}>
                    <div className="instance-config__title">
                      <I18n>Instances</I18n>
                    </div>
                  </Col>
                </Row>

                <Row className="instance-config__row">
                  <Col sm={2}>
                    <I18n className="instance-config__label">Instance Type</I18n>
                  </Col>
                  <Col sm={6}>
                    <InstanceTypeField />
                  </Col>
                </Row>

                {/* put row layout inside field component as device info may be not always available */}
                <DeviceInfoField />

                {formData.cloudConfig.provider?.code === CloudType.aws && (
                  <Row className="instance-config__row">
                    <Col sm={12}>
                      <I18n className="instance-config__label">Instance Tags</I18n>
                      <InstanceTagsField />
                    </Col>
                  </Row>
                )}

                <AdvancedSettings expanded>
                  {cloudProviders.has(formData.cloudConfig.provider?.code as CloudType) && (
                    <Row className="instance-config__row">
                      <Col sm={2}>
                        <I18n className="instance-config__label">Assign Public IP</I18n>
                      </Col>
                      <Col sm={10}>
                        <AssignPublicIPField disabled={isEditMode} />
                      </Col>
                    </Row>
                  )}
                  {formData.cloudConfig.provider?.code === CloudType.aws && (
                    <Row className="instance-config__row">
                      <Col sm={2}>
                        <I18n className="instance-config__label">Instance Profile ARN</I18n>
                      </Col>
                      <Col sm={4}>
                        <ProfileARNField disabled={isEditMode} />
                      </Col>
                    </Row>
                  )}
                </AdvancedSettings>
              </div>
            </Grid>

            <div className="instance-config__footer-row">
              <Button className="instance-config__footer-btn" onClick={cancel}>
                <I18n>Cancel</I18n>
              </Button>
              <Button
                chevronLeft
                className="instance-config__footer-btn"
                onClick={() => submit(WizardStep.Cloud)}
              >
                <I18n>Previous</I18n>
              </Button>
              <Button
                isCTA
                className="instance-config__footer-btn"
                onClick={() => submit(WizardStep.Review)}
              >
                <I18n>Review and Launch</I18n>
              </Button>
              <Button
                isCTA
                chevronRight
                className="instance-config__footer-btn"
                onClick={() => submit(WizardStep.Db)}
              >
                <I18n>Next</I18n>
              </Button>
            </div>

            {/*
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
            */}
          </FormProvider>
        </div>
        <div className="wizard-step-wrapper__col-summary">
          <Summary formData={{ ...formData, instanceConfig: formMethods.getValues() }} />
        </div>
      </div>
    </div>
  );
};
