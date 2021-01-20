import _ from 'lodash';
import { useContext, useState } from 'react';
import { browserHistory } from 'react-router';
import { ClusterOperation, WizardContext, WizardStepsFormData } from '../../UniverseWizard';
import {
  CloudType,
  ClusterType,
  FlagsArray,
  FlagsObject,
  PlacementAZ,
  PlacementRegion,
  UniverseConfigure,
  UniverseDetails
} from '../../../../helpers/dtos';
import { PlacementUI } from '../../fields/PlacementsField/PlacementsField';
import { api, QUERY_KEY } from '../../../../helpers/api';
import { useWhenMounted } from '../../../../helpers/hooks';

const tagsToArray = (tags?: FlagsObject): FlagsArray => {
  const result = [];
  if (tags) {
    for (const [name, value] of Object.entries(tags)) {
      result.push({ name, value });
    }
  }
  return result;
};

const getPlacements = (formData: WizardStepsFormData): PlacementRegion[] => {
  // remove gaps from placements list
  const placements: NonNullable<PlacementUI>[] = _.cloneDeep(
    _.compact(formData.cloudConfig.placements)
  );

  // update leaders flag "isAffinitized" in placements basing on form value
  const leaders = new Set(formData.dbConfig.preferredLeaders.flatMap((item) => item.uuid));
  placements.forEach((item) => (item.isAffinitized = leaders.has(item.uuid)));

  // prepare placements basing on user selection
  const regionMap: Record<string, PlacementRegion> = {};
  placements.forEach((item) => {
    const zone: PlacementAZ = _.omit(item, [
      'parentRegionId',
      'parentRegionCode',
      'parentRegionName'
    ]);
    if (Array.isArray(regionMap[item.parentRegionId]?.azList)) {
      regionMap[item.parentRegionId].azList.push(zone);
    } else {
      regionMap[item.parentRegionId] = {
        uuid: item.parentRegionId,
        code: item.parentRegionCode,
        name: item.parentRegionName,
        azList: [zone]
      };
    }
  });

  return Object.values(regionMap);
};

// fix some skewed/missing fields in config response
const patchConfigResponse = (
  response: UniverseDetails,
  original: UniverseDetails
) => {
  const clusterIndex = 0; // TODO: change to dynamic when support async clusters

  response.clusterOperation = original.clusterOperation;
  response.currentClusterType = original.currentClusterType;
  response.encryptionAtRestConfig = original.encryptionAtRestConfig;

  const userIntent = response.clusters[clusterIndex].userIntent;
  userIntent.instanceTags = original.clusters[clusterIndex].userIntent.instanceTags;
  userIntent.masterGFlags = original.clusters[clusterIndex].userIntent.masterGFlags;
  userIntent.tserverGFlags = original.clusters[clusterIndex].userIntent.tserverGFlags;
};

export const useLaunchUniverse = () => {
  const { formData, originalFormData, operation, universe } = useContext(WizardContext);
  const [isLoading, setIsLoading] = useState(false);
  const whenMounted = useWhenMounted();

  const launchUniverse = async () => {
    try {
      setIsLoading(true);

      switch (operation) {
        case ClusterOperation.NEW_PRIMARY: {
          // convert form data into payload suitable for the configure api call
          const configurePayload: UniverseConfigure = {
            clusterOperation: 'CREATE',
            currentClusterType: ClusterType.PRIMARY,
            rootCA: formData.securityConfig.rootCA,
            userAZSelected: false,
            communicationPorts: formData.dbConfig.communicationPorts,
            encryptionAtRestConfig: {
              key_op: formData.securityConfig.enableEncryptionAtRest ? 'ENABLE' : 'UNDEFINED'
            },
            extraDependencies: {
              installNodeExporter: formData.hiddenConfig.installNodeExporter
            },
            clusters: [
              {
                clusterType: ClusterType.PRIMARY,
                userIntent: {
                  universeName: formData.cloudConfig.universeName,
                  provider: formData.cloudConfig.provider?.uuid as string,
                  providerType: formData.cloudConfig.provider?.code as CloudType,
                  regionList: formData.cloudConfig.regionList,
                  numNodes: formData.cloudConfig.totalNodes,
                  replicationFactor: formData.cloudConfig.replicationFactor,
                  instanceType: formData.instanceConfig.instanceType as string,
                  deviceInfo: formData.instanceConfig.deviceInfo,
                  instanceTags: tagsToArray(formData.instanceConfig.instanceTags),
                  assignPublicIP: formData.instanceConfig.assignPublicIP,
                  awsArnString: formData.instanceConfig.awsArnString || '',
                  ybSoftwareVersion: formData.dbConfig.ybSoftwareVersion,
                  masterGFlags: tagsToArray(formData.dbConfig.masterGFlags),
                  tserverGFlags: tagsToArray(formData.dbConfig.tserverGFlags),
                  enableNodeToNodeEncrypt: formData.securityConfig.enableNodeToNodeEncrypt,
                  enableClientToNodeEncrypt: formData.securityConfig.enableClientToNodeEncrypt,
                  accessKeyCode: formData.hiddenConfig.accessKeyCode,
                  enableYSQL: formData.hiddenConfig.enableYSQL,
                  useTimeSync: formData.hiddenConfig.useTimeSync
                }
              }
            ]
          };

          if (
            formData.securityConfig.enableEncryptionAtRest &&
            formData.securityConfig.kmsConfig &&
            configurePayload.encryptionAtRestConfig
          ) {
            configurePayload.encryptionAtRestConfig.configUUID = formData.securityConfig.kmsConfig;
          }

          // in create mode there's no "nodeDetailsSet" yet, so make a configure call without placements to generate it
          const interimConfigure = await api.universeConfigure(
            QUERY_KEY.universeConfigure,
            configurePayload
          );
          patchConfigResponse(interimConfigure, configurePayload as UniverseDetails);

          // replace node placements returned by a configure call with one provided by the user
          interimConfigure.clusters[0].placementInfo = {
            cloudList: [
              {
                uuid: formData.cloudConfig.provider?.uuid as string,
                code: formData.cloudConfig.provider?.code as CloudType,
                regionList: getPlacements(formData)
              }
            ]
          };

          // make one more configure call to validate payload before submitting
          const finalPayload = await api.universeConfigure(
            QUERY_KEY.universeConfigure,
            interimConfigure
          );
          patchConfigResponse(finalPayload, configurePayload as UniverseDetails);

          // now everything is ready to create universe
          await api.universeCreate(finalPayload);
          break;
        }

        case ClusterOperation.EDIT_PRIMARY: {
          if (universe && !_.isEqual(formData, originalFormData)) {
            const payload = universe.universeDetails;
            payload.clusterOperation = 'EDIT';
            payload.currentClusterType = ClusterType.PRIMARY;
            payload.userAZSelected = formData.hiddenConfig.userAZSelected;

            // update props allowed to edit by form values
            const userIntent = payload.clusters[0].userIntent;
            userIntent.regionList = formData.cloudConfig.regionList;
            userIntent.numNodes = formData.cloudConfig.totalNodes;
            userIntent.instanceType = formData.instanceConfig.instanceType!;
            userIntent.instanceTags = tagsToArray(formData.instanceConfig.instanceTags);
            userIntent.deviceInfo = formData.instanceConfig.deviceInfo;

            // update placements and leaders
            payload.clusters[0].placementInfo.cloudList[0].regionList = getPlacements(formData);

            // submit configure call to validate the payload
            const finalPayload = await api.universeConfigure(QUERY_KEY.universeConfigure, payload);
            patchConfigResponse(finalPayload, payload);

            // TODO: detect if full move is going to happen by analyzing finalPayload.nodeDetailsSet
            // TODO: maybe consider universe.universeDetails.updateInProgress to avoid edits while it's true

            await api.universeEdit(finalPayload, universe.universeUUID);
          } else {
            console.log('Nothing to update - no fields changed');
          }
          break;
        }
      }
    } catch (error) {
      console.error(error);
    } finally {
      whenMounted(() => setIsLoading(false));
      browserHistory.push('/universes');
    }
  };

  return { isLaunchingUniverse: isLoading, launchUniverse };
};
