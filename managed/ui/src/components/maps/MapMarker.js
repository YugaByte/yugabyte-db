// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import PropTypes from 'prop-types';
import { Marker, Popup, Tooltip } from 'react-leaflet';
import { Icon, divIcon } from 'leaflet';
import {
  DefaultMarkerIcon,
  DefaultMarkerShadowIcon,
  RootMarkerIconBig,
  RootMarkerShadowIcon,
  ReadReplicaMarkerIconBig
} from './images';

export default class MapMarker extends Component {
  static propTypes = {
    latitude: PropTypes.number.isRequired,
    longitude: PropTypes.number.isRequired,
    type: PropTypes.oneOf([
      'Default',
      'ReadReplica',
      'AZMarker',
      'Region',
      'Table',
      'Universe',
      'All'
    ])
  };

  static defaultProps = {
    type: 'All',
    label: ''
  };

  render() {
    const { latitude, longitude, label, type, labelType, numChildren } = this.props;
    let popup;
    if (label) {
      popup = (
        <Popup>
          <span>{label}</span>
        </Popup>
      );
    }

    if (labelType === 'tooltip') {
      popup = (
        <Tooltip>
          <div>{label}</div>
        </Tooltip>
      );
    }

    const opts = {};
    if (type === 'Default') {
      opts['icon'] = new Icon({
        iconUrl: DefaultMarkerIcon,
        shadowUrl: DefaultMarkerShadowIcon,
        iconSize: [20, 27],
        popupAnchor: [10, 10],
        iconAnchor: [10, 30],
        shadowAnchor: [12, 46]
      });
    } else if (type === 'AZMarker') {
      opts['icon'] = new Icon({
        iconUrl: RootMarkerIconBig,
        shadowUrl: RootMarkerShadowIcon,
        iconSize: [35, 35],
        shadowSize: [40, 42],
        popupAnchor: [10, -4],
        iconAnchor: [12, 20],
        shadowAnchor: [5, 25]
      });
    } else if (type === 'ReadReplica') {
      opts['icon'] = new Icon({
        iconUrl: ReadReplicaMarkerIconBig,
        shadowUrl: RootMarkerShadowIcon,
        iconSize: [35, 35],
        shadowSize: [40, 42],
        popupAnchor: [10, -4],
        iconAnchor: [12, 20],
        shadowAnchor: [5, 25]
      });
    } else if (type === 'Region') {
      opts['icon'] = divIcon({
        className: 'marker-cluster-small provider-marker-cluster',
        html: numChildren
      });
    } else {
      const markerData = RootMarkerIconBig;
      opts['icon'] = new Icon({
        iconUrl: markerData,
        shadowUrl: '',
        iconSize: [35, 35],
        popupAnchor: [10, 10],
        iconAnchor: [10, 30],
        shadowAnchor: [12, 46]
      });
    }
    return (
      <Marker position={[latitude, longitude]} {...opts}>
        {popup}
      </Marker>
    );
  }
}
