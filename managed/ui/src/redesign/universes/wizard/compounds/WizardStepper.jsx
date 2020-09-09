import React from 'react';
import './WizardStepper.scss';

// steps - array of step react nodes
// activeStep - index of active step

export const WizardStepper = ({ steps, activeStep }) => {
  return (
    <div className="wizard-stepper">
      {steps.map((step, index) => (
        <div className={`wizard-stepper__item ${index === activeStep ? 'wizard-stepper__item--active' : ''}`} key={index}>
          {step}
        </div>
      ))}
    </div>
  );
};
