import _ from 'lodash';
import React, { FC } from 'react';
import { useQuery } from 'react-query';
import { api, QUERY_KEY } from '../helpers/api';
import { ClusterOperation, UniverseWizard, WizardStepsFormData } from './wizard/UniverseWizard';
import { YBErrorIndicator, YBLoading } from '../../components/common/indicators';
import { Cluster, Universe } from '../helpers/dtos';
import { WizardStep } from './wizard/compounds/WizardStepper/WizardStepper';
import { getCluster, getFormData } from './dtoToFormData';

// there are cases when API returns broken universe, so let's have simple shallow validation of its shape
const isValidUniverse = (universe?: Universe): boolean => {
  return (
    !!universe &&
    !_.isEmpty(universe?.universeDetails?.clusters) &&
    universe.universeDetails.clusters.every((item) => !_.isEmpty(item.placementInfo))
  );
};

interface EditUniverseProps {
  // TODO: use RouteComponentProps<RouteParams> when migrate to latest react-router-dom
  params: {
    // injected by router
    universeId: string;
    asyncClusterId?: string;
    wizardStep?: string;
  };
  location: Location;
}

export const EditUniverse: FC<EditUniverseProps> = ({
  params: { universeId, asyncClusterId },
  location
}) => {
  const { isLoading, isSuccess, data: universe } = useQuery(
    [QUERY_KEY.fetchUniverse, universeId],
    api.fetchUniverse
  );

  if (isSuccess && !isLoading && universe && isValidUniverse(universe)) {
    let operation: ClusterOperation;
    let cluster: Cluster | undefined;

    if (location.pathname.includes('/edit/primary')) {
      operation = ClusterOperation.EDIT_PRIMARY;
      cluster = getCluster(universe, false);
    } else {
      operation = ClusterOperation.EDIT_ASYNC;
      // get async cluster either by ID from route params or just take first
      // available (when there's no support for multiple async replicas)
      cluster = getCluster(universe, asyncClusterId || true);
    }

    const formData: WizardStepsFormData = getFormData(universe, cluster);

    return (
      <UniverseWizard
        step={WizardStep.Cloud}
        universe={universe}
        cluster={cluster}
        operation={operation}
        formData={formData}
      />
    );
  }

  if (isLoading) return <YBLoading />;

  // default to error state if failed to load/validate universe
  return <YBErrorIndicator type="universe" uuid={universeId} />;
};
