import React, { FC } from 'react';
import { ClusterOperation, UniverseWizard } from './wizard/UniverseWizard';
import { WizardStep } from './wizard/compounds/WizardStepper/WizardStepper';
import { defaultFormData } from './dtoToFormData';

interface CreateUniverseProps {
  // injected by router
  params: {
    universeId?: string;
  };
}

export const CreateUniverse: FC<CreateUniverseProps> = ({ params: { universeId } }) => {
  const operation = universeId ? ClusterOperation.NEW_ASYNC : ClusterOperation.NEW_PRIMARY;
  return (
    <UniverseWizard
      step={WizardStep.Cloud}
      // universe={...} // TODO: set parent universe when creating new async cluster
      operation={operation}
      formData={defaultFormData}
    />
  );
};
