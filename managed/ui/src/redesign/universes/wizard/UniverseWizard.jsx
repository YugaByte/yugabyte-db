import React, { useState } from 'react';
import { WizardStepper } from './compounds/WizardStepper';
import { Summary } from './compounds/Summary';
import { Button } from '../../uikit/Button/Button';
import { I18n } from '../../uikit/I18n/I18n';
import { CloudConfig } from './CloudConfig';
import { InstanceConfig } from './InstanceConfig';
import { DBConfig } from './DBConfig';
import { SecurityConfig } from './SecurityConfig';
import './UniverseWizard.scss';

const steps = [
  <I18n>1. Cloud Config</I18n>,
  <I18n>2. Instance Config</I18n>,
  <I18n>3. DB Config</I18n>,
  <I18n>4. Security Config</I18n>
];

export const UniverseWizard = ({ id }) => {
  const [activeStep, setActiveStep] = useState(0);

  const nextStep = () => activeStep < steps.length - 1 && setActiveStep(activeStep + 1);
  const prevStep = () => activeStep > 0 && setActiveStep(activeStep - 1);

  return (
    <div className="universe-wizard">
      <div className="universe-wizard__title">
        {id ? <I18n>Edit Universe</I18n> : <I18n>Create Universe</I18n>} - {id || <I18n>n/a</I18n>}
      </div>

      <WizardStepper steps={steps} activeStep={activeStep} />

      <div className="universe-wizard__container">
        <div className="universe-wizard__form">
          {activeStep === 0 && <CloudConfig />}
          {activeStep === 1 && <InstanceConfig />}
          {activeStep === 2 && <DBConfig />}
          {activeStep === 3 && <SecurityConfig />}
        </div>
        <div className="universe-wizard__summary">
          <Summary />
        </div>
      </div>

      <Button onClick={prevStep} disabled={activeStep <= 0}><I18n>Prev</I18n></Button>
      <Button onClick={nextStep} disabled={activeStep >= steps.length - 1}><I18n>Next</I18n></Button>
    </div>
  );
};
