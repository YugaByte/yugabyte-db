// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { isObject } from 'lodash';
import { OnPremConfiguration } from '../../config';
import {
  createInstanceType,
  createInstanceTypeResponse,
  createRegion,
  createRegionResponse,
  createZones,
  createZonesResponse,
  createNodeInstances,
  createNodeInstancesResponse,
  createAccessKey,
  createAccessKeyResponse,
  createAccessKeyFailure,
  resetProviderBootstrap,
  fetchCloudMetadata,
  getProviderList,
  getProviderListResponse,
  resetOnPremConfigData,
  setOnPremConfigData,
  createOnPremProvider,
  createOnPremProviderResponse
} from '../../../actions/cloud';
import { isNonEmptyArray } from '../../../utils/ObjectUtils';
import { destroy } from 'redux-form';

const mapStateToProps = (state) => {
  return {
    cloud: state.cloud,
    configuredProviders: state.cloud.providers,
    configuredRegions: state.cloud.supportedRegionList,
    accessKeys: state.cloud.accessKeys,
    cloudBootstrap: state.cloud.bootstrap,
    onPremJsonFormData: state.cloud.onPremJsonFormData
  };
};

const mapDispatchToProps = (dispatch) => {
  return {
    onPremConfigSuccess: () => {
      dispatch(fetchCloudMetadata());
      dispatch(resetProviderBootstrap());
    },

    createOnPremAccessKeys: (providerUUID, regionsMap, config) => {
      if (isObject(config) && isNonEmptyArray(config.regions) && isObject(config.key)) {
        dispatch(
          createAccessKey(providerUUID, regionsMap[config.regions[0].code], config.key)
        ).then((response) => {
          if (response.error) {
            dispatch(createAccessKeyFailure(response.payload));
          } else {
            dispatch(createAccessKeyResponse(response.payload));
          }
        });
      }
    },

    createOnPremInstanceTypes: (providerType, providerUUID, config, isEdit) => {
      if (isObject(config) && isNonEmptyArray(config.instanceTypes)) {
        config.instanceTypes.forEach((type) => {
          if ((isEdit && type.isBeingEdited) || !isEdit) {
            dispatch(createInstanceType(providerType, providerUUID, type)).then((response) => {
              dispatch(createInstanceTypeResponse(response.payload));
            });
          }
        });
      }
    },

    createOnPremProvider: (providerType, config) => {
      dispatch(
        createOnPremProvider(providerType, config.provider.name, config.provider.config)
      ).then((response) => {
        dispatch(createOnPremProviderResponse(response.payload));
      });
    },

    createOnPremRegions: (providerUUID, config, isEdit) => {
      if (isObject(config) && isNonEmptyArray(config.regions)) {
        config.regions.forEach((region) => {
          const formValues = {
            code: region.code,
            hostVPCId: '',
            name: region.code,
            latitude: region.latitude,
            longitude: region.longitude
          };
          if ((isEdit && region.isBeingEdited) || !isEdit) {
            dispatch(createRegion(providerUUID, formValues)).then((response) => {
              dispatch(createRegionResponse(response.payload));
            });
          }
        });
      }
    },

    createOnPremZones: (providerUUID, regionsMap, config, isEdit) => {
      if (isObject(config) && isNonEmptyArray(config.regions)) {
        config.regions.forEach((region) => {
          if ((isEdit && region.isBeingEdited) || !isEdit) {
            if (isObject(region) && isNonEmptyArray(region.zones)) {
              dispatch(createZones(providerUUID, regionsMap[region.code], region.zones)).then(
                (response) => {
                  dispatch(createZonesResponse(response.payload));
                }
              );
            }
          }
        });
      }
    },

    createOnPremNodes: (zonesMap, config) => {
      if (isObject(config) && isNonEmptyArray(config.nodes)) {
        // Get a grouping of zones to nodes
        const zoneToNodeMap = {};
        config.nodes.forEach((node) => {
          if (isObject(node)) {
            const zoneUuid = zonesMap[node.zone];
            node.nodeName = 'yb-' + node.zone + '-n' + config.nodes.indexOf(node).toString();
            zoneToNodeMap[zoneUuid] = zoneToNodeMap[zoneUuid] || [];
            zoneToNodeMap[zoneUuid].push(node);
          }
        });
        // dispatch for each zone
        Object.keys(zoneToNodeMap).forEach((zoneUuid) => {
          dispatch(createNodeInstances(zoneUuid, zoneToNodeMap[zoneUuid])).then((response) => {
            dispatch(createNodeInstancesResponse(response.payload));
          });
        });
      }
    },

    resetConfigForm: () => {
      dispatch(destroy('onPremConfigForm'));
    },

    resetOnPremJson: () => {
      dispatch(resetOnPremConfigData());
    },
    fetchProviderList: () => {
      dispatch(getProviderList()).then((response) => {
        dispatch(getProviderListResponse(response.payload));
      });
    },
    setConfigJsonData: (payload) => {
      dispatch(setOnPremConfigData(payload));
    }
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(OnPremConfiguration);
