import React, { FC } from 'react';
import clsx from 'clsx';
import { I18n } from '../../../../uikit/I18n/I18n';
import './WizardStepper.scss';

export enum WizardStep {
  Cloud = 'cloud',
  Instance = 'instance',
  Db = 'db',
  Security = 'security',
  Review = 'review'
}

interface WizardStepperProps {
  activeStep: WizardStep;
  clickableTabs: boolean;
  onChange?(step: WizardStep): void;
}

export const wizardSteps = [
  [WizardStep.Cloud, <I18n>1. Cloud Config</I18n>],
  [WizardStep.Instance, <I18n>2. Instance Config</I18n>],
  [WizardStep.Db, <I18n>3. DB Config</I18n>],
  [WizardStep.Security, <I18n>4. Security Config</I18n>],
  [WizardStep.Review, <I18n>5. Review</I18n>]
] as const;

export const WizardStepper: FC<WizardStepperProps> = ({ activeStep, clickableTabs, onChange }) => {
  const click = (step: WizardStep) => {
    if (clickableTabs && step !== activeStep && onChange) {
      onChange(step);
    }
  };

  return (
    <div className="wizard-stepper">
      {wizardSteps.map(([step, stepComponent]) => (
        <div
          className={clsx('wizard-stepper__item', {
            'wizard-stepper__item--active': step === activeStep,
            'wizard-stepper__item--clickable': clickableTabs && step !== activeStep
          })}
          key={step}
          onClick={() => click(step)}
        >
          {stepComponent}
        </div>
      ))}
    </div>
  );
};
