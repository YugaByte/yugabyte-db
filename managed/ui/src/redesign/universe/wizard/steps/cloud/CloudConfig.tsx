import React, { Dispatch, FC, useContext } from 'react';
import { Col, Grid, Row } from 'react-bootstrap';
import { FormProvider, useForm } from 'react-hook-form';
import { I18n } from '../../../../uikit/I18n/I18n';
import { Button } from '../../../../uikit/Button/Button';
import { UniverseNameField } from '../../fields/UniverseNameField/UniverseNameField';
import { ProvidersField, ProviderUI } from '../../fields/ProvidersField/ProvidersField';
import { RegionListField } from '../../fields/RegionListField/RegionListField';
import { TotalNodesField } from '../../fields/TotalNodesField/TotalNodesField';
import { ReplicationFactorField } from '../../fields/ReplicationFactorField/ReplicationFactorField';
import { ReplicaPlacementToggleField } from '../../fields/ReplicaPlacementToggleField/ReplicaPlacementToggleField';
import { PlacementsField, PlacementUI } from '../../fields/PlacementsField/PlacementsField';
import { WizardAction, WizardContext } from '../../UniverseWizard';
import { WizardStep, WizardStepper } from '../../compounds/WizardStepper/WizardStepper';
import { Summary } from '../../compounds/Summary/Summary';
import '../StepWrapper.scss';
import './CloudConfig.scss';

export interface CloudConfigFormValue {
  universeName: string;
  provider: ProviderUI | null;
  regionList: string[]; // array of region IDs
  totalNodes: number;
  replicationFactor: number;
  autoPlacement: boolean;
  placements: PlacementUI[];
}

interface CloudConfigProps {
  dispatch: Dispatch<WizardAction>;
}

export const CloudConfig: FC<CloudConfigProps> = ({ dispatch }) => {
  const { isEditMode, originalFormData, formData } = useContext(WizardContext);
  const formMethods = useForm<CloudConfigFormValue>({
    mode: 'onChange',
    defaultValues: formData.cloudConfig
  });

  const reset = () => formMethods.reset(originalFormData.cloudConfig);

  const cancel = () => dispatch({ type: 'exit_wizard', payload: true });

  const submit = async (nextStep: WizardStep): Promise<void> => {
    const isValid = await formMethods.trigger();
    if (isValid) {
      const formValues: CloudConfigFormValue = formMethods.getValues();
      dispatch({ type: 'update_form_data', payload: { cloudConfig: formValues } });
      dispatch({ type: 'change_step', payload: nextStep });
    }
  };

  return (
    <div className="wizard-step-wrapper">
      <div className="wizard-step-wrapper__stepper">
        <WizardStepper activeStep={WizardStep.Cloud} clickableTabs={isEditMode} onChange={submit} />
      </div>
      <div className="wizard-step-wrapper__container">
        <div className="wizard-step-wrapper__col-form">
          <FormProvider {...formMethods}>
            <Grid fluid>
              <div className="cloud-config">
                <Row>
                  <Col xs={12}>
                    <div className="cloud-config__title">
                      <I18n>Cloud Configuration</I18n>
                      <div className="cloud-config__line" />
                      <div className="cloud-config__reset" onClick={reset}>
                        <I18n>Reset Configuration</I18n>
                      </div>
                    </div>
                  </Col>
                </Row>

                <Row className="cloud-config__row">
                  <Col sm={2}>
                    <I18n className="cloud-config__label">Name</I18n>
                  </Col>
                  <Col sm={8}>
                    <UniverseNameField disabled={isEditMode} />
                  </Col>
                </Row>

                <Row className="cloud-config__row">
                  <Col sm={2}>
                    <I18n className="cloud-config__label">Provider</I18n>
                  </Col>
                  <Col sm={8}>
                    <ProvidersField disabled={isEditMode} />
                  </Col>
                </Row>

                <Row className="cloud-config__row">
                  <Col sm={2}>
                    <I18n className="cloud-config__label">Regions</I18n>
                  </Col>
                  <Col sm={10}>
                    <RegionListField />
                  </Col>
                </Row>

                <Row className="cloud-config__row">
                  <Col sm={2}>
                    <I18n className="cloud-config__label">Replication Factor</I18n>
                  </Col>
                  <Col sm={10}>
                    <ReplicationFactorField disabled={isEditMode} />
                  </Col>
                </Row>

                <Row className="cloud-config__row">
                  <Col xs={12}>
                    <div className="cloud-config__title">
                      {/* TODO: show "Pod" for kuber provider */}
                      <I18n>Node Placement</I18n>
                    </div>
                  </Col>
                </Row>

                <Row className="cloud-config__row">
                  <Col sm={2}>
                    <I18n className="cloud-config__label">Replica Placement</I18n>
                  </Col>
                  <Col sm={4}>
                    <ReplicaPlacementToggleField disabled={isEditMode} />
                  </Col>
                  <Col sm={2}>
                    {/* TODO: show "Pod" for kuber provider */}
                    <I18n className="cloud-config__label cloud-config__label--right">
                      Total Nodes
                    </I18n>
                  </Col>
                  <Col sm={2}>
                    <TotalNodesField />
                  </Col>
                </Row>

                <PlacementsField />
              </div>
            </Grid>

            <div className="cloud-config__footer-row">
              <Button className="cloud-config__footer-btn" onClick={cancel}>
                <I18n>Cancel</I18n>
              </Button>
              <Button
                isCTA
                chevronRight
                className="cloud-config__footer-btn"
                onClick={() => submit(WizardStep.Instance)}
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
          <Summary formData={{ ...formData, cloudConfig: formMethods.watch() }} />
        </div>
      </div>
    </div>
  );
};
