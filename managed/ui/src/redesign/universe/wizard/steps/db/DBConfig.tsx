import React, { Dispatch, FC, useContext } from 'react';
import { FormProvider, useForm } from 'react-hook-form';
import { Col, Grid, Row } from 'react-bootstrap';
import { I18n } from '../../../../uikit/I18n/I18n';
import { Button } from '../../../../uikit/Button/Button';
import { WizardAction, WizardContext } from '../../UniverseWizard';
import { DBVersionField } from '../../fields/DBVersionField/DBVersionField';
import { AdvancedSettings } from '../../compounds/ExpandableSection/AdvancedSettings';
import { PreferredLeadersField } from '../../fields/PreferredLeadersField/PreferredLeadersField';
import { PlacementUI } from '../../fields/PlacementsField/PlacementsField';
import { MasterFlagsField } from '../../fields/MasterFlagsField/MasterFlagsField';
import { TServerFlagsField } from '../../fields/TServerFlagsField/TServerFlagsField';
import { CommunicationPorts, FlagsObject } from '../../../../helpers/dtos';
import { CommunicationPortsField } from '../../fields/CommunicationPortsField/CommunicationPortsField';
import { WizardStep, WizardStepper } from '../../compounds/WizardStepper/WizardStepper';
import { Summary } from '../../compounds/Summary/Summary';
import '../StepWrapper.scss';
import './DBConfig.scss';

export interface DBConfigFormValue {
  ybSoftwareVersion: string | null;
  preferredLeaders: NonNullable<PlacementUI>[]; // don't allow gaps in leaders array
  masterGFlags: FlagsObject;
  tserverGFlags: FlagsObject;
  communicationPorts: CommunicationPorts;
}

interface DBConfigProps {
  dispatch: Dispatch<WizardAction>;
}

export const DBConfig: FC<DBConfigProps> = ({ dispatch }) => {
  const { isEditMode, formData } = useContext(WizardContext);
  const formMethods = useForm<DBConfigFormValue>({
    mode: 'onChange',
    defaultValues: formData.dbConfig
  });

  const cancel = () => dispatch({ type: 'exit_wizard', payload: true });

  const submit = async (nextStep: WizardStep): Promise<void> => {
    const isValid = await formMethods.trigger();
    if (isValid) {
      const formValues: DBConfigFormValue = formMethods.getValues();
      dispatch({ type: 'update_form_data', payload: { dbConfig: formValues } });
      dispatch({ type: 'change_step', payload: nextStep });
    }
  };

  return (
    <div className="wizard-step-wrapper">
      <div className="wizard-step-wrapper__stepper">
        <WizardStepper activeStep={WizardStep.Db} clickableTabs onChange={submit} />
      </div>
      <div className="wizard-step-wrapper__container">
        <div className="wizard-step-wrapper__col-form">
          <FormProvider {...formMethods}>
            <Grid fluid>
              <div className="db-config">
                <Row>
                  <Col xs={12}>
                    <div className="db-config__title">
                      <I18n>DB Configuration</I18n>
                    </div>
                  </Col>
                </Row>

                <Row className="db-config__row">
                  <Col sm={2}>
                    <I18n className="db-config__label">DB Version</I18n>
                  </Col>
                  <Col sm={6}>
                    <DBVersionField disabled={isEditMode} />
                  </Col>
                </Row>

                <Row className="db-config__row">
                  <Col sm={2}>
                    <I18n className="db-config__label">Preferred Leaders</I18n>
                  </Col>
                  <Col sm={10}>
                    <PreferredLeadersField />
                  </Col>
                </Row>

                <AdvancedSettings expanded={false}>
                  <Row className="db-config__row db-config__row--align-top">
                    <Col sm={2}>
                      <I18n className="db-config__label db-config__label--margin-top">
                        Config Flags
                      </I18n>
                    </Col>
                    <Col sm={10}>
                      <MasterFlagsField disabled={isEditMode} />
                      <TServerFlagsField disabled={isEditMode} />
                    </Col>
                  </Row>

                  <Row className="db-config__row db-config__row--align-top">
                    <Col sm={2}>
                      <I18n className="db-config__label db-config__label--margin-top">
                        Override Ports
                      </I18n>
                    </Col>
                    <Col sm={10}>
                      <CommunicationPortsField disabled={isEditMode} />
                    </Col>
                  </Row>
                </AdvancedSettings>
              </div>
            </Grid>

            <div className="db-config__footer-row">
              <Button className="db-config__footer-btn" onClick={cancel}>
                <I18n>Cancel</I18n>
              </Button>
              <Button
                chevronLeft
                className="db-config__footer-btn"
                onClick={() => submit(WizardStep.Instance)}
              >
                <I18n>Previous</I18n>
              </Button>
              <Button
                isCTA
                className="db-config__footer-btn"
                onClick={() => submit(WizardStep.Review)}
              >
                <I18n>Review and Launch</I18n>
              </Button>
              <Button
                isCTA
                chevronRight
                className="db-config__footer-btn"
                onClick={() => submit(WizardStep.Security)}
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
          <Summary formData={formData} />
        </div>
      </div>
    </div>
  );
};
