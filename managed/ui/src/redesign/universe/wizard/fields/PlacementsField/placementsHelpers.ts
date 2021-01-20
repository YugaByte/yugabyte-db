import _ from 'lodash';
import { useQuery } from 'react-query';
import { useFormContext } from 'react-hook-form';
import { useEffect, useRef, useState } from 'react';
import { Cluster, ClusterType, UniverseConfigure } from '../../../../helpers/dtos';
import { CloudConfigFormValue } from '../../steps/cloud/CloudConfig';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { AvailabilityZoneUI, PlacementUI } from './PlacementsField';
import { ClusterOperation } from '../../UniverseWizard';
import { ProviderUI } from '../ProvidersField/ProvidersField';
import { getPlacementsFromCluster } from '../../../dtoToFormData';

const _log = (...args: any[]) => {
  // console.log(...args); // comment to turn off placement debug logs
};

// returns all available zones for provided regions and zones with the exception of current placements
export const useAvailabilityZones = (
  provider: ProviderUI | null,
  regionList: string[],
  currentPlacements: PlacementUI[]
): { zonesAll: AvailabilityZoneUI[]; zonesFiltered: AvailabilityZoneUI[] } => {
  const [zonesAll, setZonesAll] = useState<AvailabilityZoneUI[]>([]);
  const [zonesFiltered, setZonesFiltered] = useState<AvailabilityZoneUI[]>([]);

  // zones are the derivatives from regions as every region item has "zones" prop
  const { data: allRegions } = useQuery(
    [QUERY_KEY.getRegionsList, provider?.uuid],
    api.getRegionsList,
    { enabled: !!provider?.uuid } // make sure query won't run when there's no provider defined
  );

  // update all zones list on regions change
  useEffect(() => {
    const selectedRegions = new Set(regionList);

    const zones = (allRegions || [])
      .filter((region) => selectedRegions.has(region.uuid))
      .flatMap<AvailabilityZoneUI>((region) => {
        // add extra fields with parent region data
        return region.zones.map((zone) => ({
          ...zone,
          parentRegionId: region.uuid,
          parentRegionName: region.name,
          parentRegionCode: region.code
        }));
      });

    setZonesAll(_.sortBy(zones, 'name'));
  }, [regionList, allRegions]);

  // update filtered zones when all zones or current placements changed
  useEffect(() => {
    const currentPlacementsMap = _.keyBy(currentPlacements, 'uuid');
    const filtered = zonesAll.filter((item) => !currentPlacementsMap[item.uuid]);
    setZonesFiltered(filtered);
  }, [zonesAll, currentPlacements]);

  return { zonesAll, zonesFiltered };
};

// fetch auto-placements depending on current form value combination + set auto-placements to form value
export const useAutoPlacement = (
  name: string,
  operation: ClusterOperation,
  regionList: string[],
  totalNodes: number,
  replicationFactor: number,
  autoPlacement: boolean,
  zonesAll: AvailabilityZoneUI[]
) => {
  const prevZones = useRef<AvailabilityZoneUI[]>([]);
  const prevPropsCombination = useRef({ autoPlacement, regionList, totalNodes, replicationFactor });
  const { setValue, getValues } = useFormContext<CloudConfigFormValue>();
  const [needAutoPlacement, setNeedAutoPlacement] = useState(false);

  const clusterType =
    operation === ClusterOperation.NEW_PRIMARY ? ClusterType.PRIMARY : ClusterType.ASYNC;

  // TODO: check payload for case of adding new async cluster to existing universe
  const payload: UniverseConfigure = {
    currentClusterType: clusterType,
    clusterOperation: 'CREATE', // auto-placement supposed to be used for new universes only
    resetAZConfig: true,
    userAZSelected: false,
    clusters: [
      {
        userIntent: {
          numNodes: totalNodes,
          regionList,
          instanceType: '', // TODO: put default instance type per for selected provider as there's some logic about it on BE side
          replicationFactor
        }
      }
    ]
  };

  const { isFetching } = useQuery([QUERY_KEY.universeConfigure, payload], api.universeConfigure, {
    enabled:
      needAutoPlacement &&
      autoPlacement &&
      totalNodes >= replicationFactor &&
      !_.isEmpty(regionList),
    onSuccess: (data) => {
      const cluster = _.find<Cluster>(data.clusters, { clusterType }); // TODO: revise logic for async cluster case
      const zones = getPlacementsFromCluster(cluster);
      _log('loaded auto placement', zones);
      setValue(name, zones, { shouldValidate: true });
      setNeedAutoPlacement(false);
    }
  });

  // manual mode - update placements when zones (i.e. regions) or replication factor changed only
  useEffect(() => {
    if (autoPlacement) return;

    if (_.isEmpty(zonesAll)) {
      // make sure it was a reset and not initial render when zones are empty due to being loaded
      if (!_.isEmpty(prevZones.current)) {
        const newPlacements = Array<PlacementUI>(replicationFactor).fill(null);
        setValue(name, newPlacements, { shouldValidate: true });
        setNeedAutoPlacement(false);
        _log('manual: reset placements to', newPlacements);
      }
    } else {
      const currentPlacements = getValues<string, PlacementUI[]>(name);

      // filter zones and put "null" in place of zones from unavailable (i.e. deleted) regions, if any
      const zoneIDs = new Set(zonesAll.flatMap((item) => item.uuid));
      const filteredCurrentPlacements = currentPlacements.map((item) =>
        zoneIDs.has(item?.uuid || '') ? item : null
      );

      // make sure number of placement rows always equal to replication factor
      const newPlacements: PlacementUI[] = [];
      for (let i = 0; i < replicationFactor; i++) {
        newPlacements.push(filteredCurrentPlacements[i] || null);
      }

      setValue(name, newPlacements, { shouldValidate: true });
      setNeedAutoPlacement(false);

      _log('manual: update placements to', newPlacements);
    }

    // keep track of zones to identify when it was a zones reset or initial zones loading
    prevZones.current = zonesAll;
  }, [autoPlacement, replicationFactor, zonesAll]);

  // auto mode
  useEffect(() => {
    const propsCombination = { autoPlacement, regionList, totalNodes, replicationFactor };

    if (autoPlacement) {
      if (_.isEmpty(regionList)) {
        _log('auto: reset placements');
        setValue(name, [], { shouldValidate: true });
        setNeedAutoPlacement(false);
      } else {
        // need auto-placement when any of autoPlacement/regionList/totalNodes/replicationFactor changed
        // this way there's no unwanted triggering of auto-placement on navigating between wizard steps
        const needUpdate = !_.isEqual(prevPropsCombination.current, propsCombination);
        _log('auto: need auto-placement from api?', needUpdate);
        setNeedAutoPlacement(needUpdate);
      }
    }

    prevPropsCombination.current = propsCombination;
  }, [autoPlacement, regionList, totalNodes, replicationFactor]);

  // return isFetching flag as it updates even when re-running same queries
  return { isLoading: isFetching };
};
