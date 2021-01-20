import _ from 'lodash';
import { WizardStepsFormData } from './wizard/UniverseWizard';
import { CloudConfigFormValue } from './wizard/steps/cloud/CloudConfig';
import { InstanceConfigFormValue } from './wizard/steps/instance/InstanceConfig';
import { DBConfigFormValue } from './wizard/steps/db/DBConfig';
import { SecurityConfigFormValue } from './wizard/steps/security/SecurityConfig';
import { HiddenConfig } from './wizard/steps/review/Review';
import {
  Cluster,
  ClusterType,
  FlagsObject,
  PlacementCloud,
  PlacementRegion,
  Universe
} from '../helpers/dtos';
import { PlacementUI } from './wizard/fields/PlacementsField/PlacementsField';
import { defaultPorts } from './wizard/fields/CommunicationPortsField/CommunicationPortsEditor';

const DEFAULT_CLOUD_CONFIG: CloudConfigFormValue = {
  universeName: '',
  provider: null,
  regionList: [],
  totalNodes: 3,
  replicationFactor: 3,
  autoPlacement: true, // "AUTO" is the default value when creating new universe
  placements: []
};

const DEFAULT_INSTANCE_CONFIG: InstanceConfigFormValue = {
  instanceType: null,
  deviceInfo: null,
  instanceTags: {},
  assignPublicIP: true,
  awsArnString: ''
};

const DEFAULT_DB_CONFIG: DBConfigFormValue = {
  ybSoftwareVersion: null,
  preferredLeaders: [],
  masterGFlags: {},
  tserverGFlags: {},
  communicationPorts: defaultPorts
};

const DEFAULT_SECURITY_CONFIG: SecurityConfigFormValue = {
  enableAuthentication: false,
  enableNodeToNodeEncrypt: false,
  enableClientToNodeEncrypt: false,
  enableEncryptionAtRest: false,
  rootCA: null,
  kmsConfig: null
};

const DEFAULT_HIDDEN_CONFIG: HiddenConfig = {
  accessKeyCode: null,
  enableYSQL: true,
  userAZSelected: false,
  useTimeSync: true,
  installNodeExporter: true
};

export const defaultFormData: WizardStepsFormData = {
  cloudConfig: DEFAULT_CLOUD_CONFIG,
  instanceConfig: DEFAULT_INSTANCE_CONFIG,
  dbConfig: DEFAULT_DB_CONFIG,
  securityConfig: DEFAULT_SECURITY_CONFIG,
  hiddenConfig: DEFAULT_HIDDEN_CONFIG
};

// return either primary or async cluster (by ID or first of a type)
export const getCluster = (
  universe: Universe,
  asyncCluster: string | boolean
): Cluster | undefined => {
  if (asyncCluster) {
    if (asyncCluster === true) {
      return _.find<Cluster>(universe.universeDetails.clusters, { clusterType: ClusterType.ASYNC });
    } else {
      return _.find<Cluster>(universe.universeDetails.clusters, { uuid: asyncCluster });
    }
  } else {
    return _.find<Cluster>(universe.universeDetails.clusters, { clusterType: ClusterType.PRIMARY });
  }
};

// return placements from provided cluster
export const getPlacementsFromCluster = (
  cluster?: Cluster,
  providerId?: string // allocated for the future, currently there's single cloud per universe only
): PlacementUI[] => {
  let placements: PlacementUI[] = [];

  if (cluster?.placementInfo.cloudList) {
    let regions: PlacementRegion[];
    if (providerId) {
      // find exact cloud corresponding to provider ID and take its regions
      const cloud = _.find<PlacementCloud>(cluster.placementInfo.cloudList, { uuid: providerId });
      regions = cloud?.regionList || [];
    } else {
      // concat all regions for all available clouds
      regions = cluster.placementInfo.cloudList.flatMap((item) => item.regionList);
    }

    // add extra fields with parent region data
    placements = regions.flatMap<PlacementUI>((region) => {
      return region.azList.map((zone) => ({
        ...zone,
        parentRegionId: region.uuid,
        parentRegionName: region.name,
        parentRegionCode: region.code
      }));
    });

    // ensure that placements number always same as replication factor
    while (placements.length < cluster.userIntent.replicationFactor) {
      placements.push(null);
    }
  } else {
    console.error('Error on extracting placements from cluster', cluster);
  }

  return _.sortBy(placements, 'name');
};

export const AUTH_GFLAG_YSQL = 'ysql_enable_auth';
export const AUTH_GFLAG_YCQL = 'use_cassandra_authentication';

// transform cluster DTO into a format suitable for wizard forms
export const getFormData = (universe?: Universe, cluster?: Cluster): WizardStepsFormData => {
  const formData: WizardStepsFormData = _.cloneDeep(defaultFormData);

  if (universe && cluster) {
    // if we are here - it's edit mode
    formData.cloudConfig.universeName = cluster.userIntent.universeName;
    formData.cloudConfig.provider = {
      uuid: cluster.userIntent.provider,
      code: cluster.userIntent.providerType
    };
    formData.cloudConfig.regionList = cluster.userIntent.regionList;
    formData.cloudConfig.totalNodes = cluster.userIntent.numNodes;
    formData.cloudConfig.replicationFactor = cluster.userIntent.replicationFactor;
    formData.cloudConfig.autoPlacement = false; // must be always "false" for editing existing universe
    formData.cloudConfig.placements = getPlacementsFromCluster(cluster);

    formData.instanceConfig.instanceType = cluster.userIntent.instanceType;
    formData.instanceConfig.deviceInfo = cluster.userIntent.deviceInfo;
    formData.instanceConfig.instanceTags = cluster.userIntent.instanceTags as FlagsObject;
    formData.instanceConfig.assignPublicIP = cluster.userIntent.assignPublicIP;
    formData.instanceConfig.awsArnString = cluster.userIntent.awsArnString;

    formData.dbConfig.ybSoftwareVersion = cluster.userIntent.ybSoftwareVersion;
    formData.dbConfig.preferredLeaders = _.filter(
      _.compact<NonNullable<PlacementUI>>(formData.cloudConfig.placements),
      { isAffinitized: true }
    );
    formData.dbConfig.masterGFlags = cluster.userIntent.masterGFlags as FlagsObject;
    formData.dbConfig.tserverGFlags = cluster.userIntent.tserverGFlags as FlagsObject;
    formData.dbConfig.communicationPorts = universe.universeDetails.communicationPorts;

    formData.securityConfig.enableAuthentication =
      cluster.userIntent.tserverGFlags[AUTH_GFLAG_YSQL] === 'true' ||
      cluster.userIntent.tserverGFlags[AUTH_GFLAG_YCQL] === 'true';
    formData.securityConfig.enableClientToNodeEncrypt =
      cluster.userIntent.enableClientToNodeEncrypt;
    formData.securityConfig.enableNodeToNodeEncrypt = cluster.userIntent.enableNodeToNodeEncrypt;
    formData.securityConfig.rootCA = universe.universeDetails.rootCA;
    formData.securityConfig.enableEncryptionAtRest =
      universe.universeDetails.encryptionAtRestConfig.encryptionAtRestEnabled;
    formData.securityConfig.kmsConfig =
      universe.universeDetails.encryptionAtRestConfig.kmsConfigUUID;

    formData.hiddenConfig.accessKeyCode = cluster.userIntent.accessKeyCode;
    formData.hiddenConfig.enableYSQL = cluster.userIntent.enableYSQL;
    formData.hiddenConfig.userAZSelected = false;
    formData.hiddenConfig.useTimeSync = cluster.userIntent.useTimeSync;
    formData.hiddenConfig.installNodeExporter =
      universe.universeDetails.extraDependencies.installNodeExporter;
  }

  return formData;
};
