// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Marker, FeatureGroup, Polygon } from 'react-leaflet';
import { divIcon } from 'leaflet';
import MapMarker from './MapMarker';
import { getPointsOnCircle } from '../../utils/ObjectUtils';
import './stylesheets/universeRegionMarkerLayer.scss';
import {
  getPrimaryCluster,
  getReadOnlyCluster,
  getPlacementRegions
} from '../../utils/UniverseUtils';
import { isNonEmptyObject, isNonEmptyArray } from '../../utils/ObjectUtils';

export default class UniverseRegionMarkerLayer extends Component {
  getMarkers = (clusters, type) => {
    const cluster = type === 'primary' ? getPrimaryCluster(clusters) : getReadOnlyCluster(clusters);
    const markerType = type === 'primary' ? 'AZMarker' : 'ReadReplica';
    if (!isNonEmptyObject(cluster)) {
      return null;
    }
    const placementRegions = getPlacementRegions(cluster);
    const clusterRegions = cluster.regions;
    const azMarkerPoints = [];
    const markers = [];
    placementRegions.forEach(function (regionItem, regionIdx) {
      const regionMarkerIcon = divIcon({ className: 'universe-region-marker' });
      const currentRegion = clusterRegions.find((region) => region.uuid === regionItem.uuid);
      const regionLatLong = [currentRegion.latitude, currentRegion.longitude];
      const azPoints = getPointsOnCircle(regionItem.azList.length, regionLatLong, 2);
      azPoints.forEach(function (azPoint) {
        azMarkerPoints.push(azPoint);
      });
      azPoints.forEach(function (azItem, azIdx) {
        const label = (
          <span>
            <div>Region: {regionItem.name}</div>
            <div>Availability Zone: {regionItem.azList[azIdx].name}</div>
            <div>Nodes: {regionItem.azList[azIdx].numNodesInAZ}</div>
          </span>
        );
        markers.push(
          <MapMarker
            key={'az-marker-' + type + '-' + regionIdx + azIdx}
            type={markerType}
            latitude={azItem[0]}
            longitude={azItem[1]}
            label={label}
            labelType={'tooltip'}
          />
        );
      });
      markers.push(
        <Marker
          key={'region-marker-' + type + '-' + regionIdx}
          position={[currentRegion.latitude, currentRegion.longitude]}
          icon={regionMarkerIcon}
        />
      );
    });

    markers.push(
      <Polygon
        key={type + 'az-line-polygon'}
        color="#A9A9A9"
        fillColor="transparent"
        positions={azMarkerPoints}
      />
    );
    return markers;
  };

  render() {
    const {
      universe: {
        universeDetails: { clusters }
      }
    } = this.props;
    let regionMarkers = this.getMarkers(clusters, 'primary');
    const readreplicaMarkers = this.getMarkers(clusters, 'async');
    if (isNonEmptyArray(readreplicaMarkers)) {
      regionMarkers = regionMarkers.concat(readreplicaMarkers);
    }
    return (
      <div>
        <FeatureGroup>{regionMarkers}</FeatureGroup>
      </div>
    );
  }
}
