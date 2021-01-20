// Copyright (c) YugaByte, Inc.

import Leaflet from 'leaflet';
import { Media } from 'react-bootstrap';
import { Link } from 'react-router';
import { MapLayer } from 'react-leaflet';
import { isObject } from 'lodash';
import 'leaflet.markercluster/dist/MarkerCluster.css';
import {
  isNonEmptyArray,
  isValidObject,
  sortByLengthOfArrayProperty
} from '../../utils/ObjectUtils';
import React, { Component } from 'react';
import './stylesheets/MarkerClusterLayer.scss';
import ReactDOMServer from 'react-dom/server';

require('leaflet.markercluster');

class MarkerDetail extends Component {
  render() {
    const { markerDetail } = this.props;
    if (!isValidObject(markerDetail)) {
      return <span />;
    }

    let universeCount = 0;
    let markerDetailUniverseLinks = null;
    const providerName = isObject(markerDetail.provider) ? markerDetail.provider.name : '';
    const markerListItems = [
      { name: 'provider', data: providerName },
      { name: 'region', data: markerDetail.name }
    ];
    if (isNonEmptyArray(markerDetail.universes)) {
      universeCount = markerDetail.universes.length;
      markerDetailUniverseLinks = markerDetail.universes.map((universe, index) => (
        <Link key={index} to={'/universes/' + universe.universeUUID}>
          {universe.name}
        </Link>
      ));
      markerListItems.push({
        name: 'universes',
        data: markerDetail.universes.map((universeItem, universeIdx) => ({
          name: universeItem.name
        }))
      });
    }

    return (
      <Media className="marker-detail">
        <Media.Left>
          {universeCount ? (
            <div className="marker-cluster-small">{markerDetail.universes.length}</div>
          ) : (
            <div className="marker-cluster-small marker-cluster-outline">&nbsp;</div>
          )}
        </Media.Left>
        <Media.Body>
          {universeCount ? (
            <div className="marker-universe-names">{markerDetailUniverseLinks}</div>
          ) : (
            ''
          )}
          <div className="marker-region-name">{markerDetail.name}</div>
          <div className="marker-provider-name">{markerDetail.provider.name}</div>
        </Media.Body>
      </Media>
    );
  }
}

export default class MarkerClusterLayer extends MapLayer {
  createLeafletElement(props) {
    // Needed to react-leaflet^1.1.1 when extending Base Map Classes
  }

  UNSAFE_componentWillMount() {
    super.componentWillMount();
    this.leafletElement = Leaflet.markerClusterGroup({
      zoomToBoundsOnClick: false,
      spiderfyOnMaxZoom: false,
      singleMarkerMode: true,
      disableClusteringAtZoom: 3,
      maxClusterRadius: 20,

      iconCreateFunction: function (cluster) {
        const markers = cluster.getAllChildMarkers();
        let universeCount = 0;
        markers.forEach(function (marker) {
          if (marker.ybData.universes) {
            universeCount += marker.ybData.universes.length;
          }
        });
        const clusterIconData = universeCount
          ? { className: 'marker-cluster-small', html: universeCount.toString() }
          : { className: 'marker-cluster-small marker-cluster-outline', html: '&nbsp;' };
        return new Leaflet.DivIcon(clusterIconData);
      }
    });
  }

  UNSAFE_componentWillReceiveProps(nextProps) {
    const self = this;
    this.leafletElement.clearLayers();
    const { newMarkerData } = nextProps;
    if (newMarkerData.length > 0) {
      const newMarkers = [];
      sortByLengthOfArrayProperty(newMarkerData, 'universes').forEach((obj) => {
        const popupDetail = ReactDOMServer.renderToString(<MarkerDetail markerDetail={obj} />);
        const latLng = Leaflet.latLng(obj.latitude, obj.longitude);
        const leafletMarker = new Leaflet.Marker(latLng).bindPopup(popupDetail, {
          maxHeight: 100,
          maxWidth: 300,
          minWidth: 100
        });
        leafletMarker.ybData = obj;
        newMarkers.push(leafletMarker);
      });

      this.leafletElement.addLayers(newMarkers);
    }
    self.leafletElement.on('clusterclick', function (a) {
      let clusterMarker = '';
      a.layer.getAllChildMarkers().forEach(function (markerItem, markerIndex) {
        clusterMarker += `${ReactDOMServer.renderToString(
          <MarkerDetail markerDetail={markerItem._popup._source.ybData} />
        )}`;
      });
      const marker = a.layer.getAllChildMarkers()[0];
      const cluster = a.target.getVisibleParent(marker);
      cluster.bindPopup(clusterMarker, { minWidth: 100 }).openPopup();
    });
  }

  shouldComponentUpdate() {
    return false;
  }

  render() {
    return null;
  }
}
