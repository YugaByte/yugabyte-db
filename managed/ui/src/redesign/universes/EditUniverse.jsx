import React from 'react';
import { UniverseWizard } from './wizard/UniverseWizard';

// params - part of props injected by router
export const EditUniverse = ({ params }) => {
  return (
    <div>
      <UniverseWizard id={params.uuid} />
    </div>
  );
};
