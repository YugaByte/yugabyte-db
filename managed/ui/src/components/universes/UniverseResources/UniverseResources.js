// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { isNonEmptyObject } from '../../../utils/ObjectUtils';
import { YBResourceCount, YBCost } from '../../../components/common/descriptors';
import PropTypes from 'prop-types';
import { FlexContainer, FlexShrink, FlexGrow } from '../../common/flexbox/YBFlexBox';
import './UniverseResources.scss';

export default class UniverseResources extends Component {
  static propTypes = {
    renderType: PropTypes.oneOf(['Display', 'Configure'])
  };

  static defaultProps = {
    renderType: 'Configure'
  };

  render() {
    const { resources, renderType } = this.props;
    let empty = true;
    let costPerDay = '$0.00';
    let costPerMonth = '$0.00';
    let numCores = 0;
    let memSizeGB = 0;
    let volumeSizeGB = 0;
    let volumeCount = 0;
    let universeNodes = <span />;
    let renderCosts = false;
    if (isNonEmptyObject(resources)) {
      empty = false;
      renderCosts = Number(resources.pricePerHour) > 0;
      costPerDay = <YBCost value={resources.pricePerHour} multiplier={'day'} />;
      costPerMonth = <YBCost value={resources.pricePerHour} multiplier={'month'} />;
      numCores = resources.numCores;
      memSizeGB = resources.memSizeGB ? resources.memSizeGB : 0;
      volumeSizeGB = resources.volumeSizeGB ? resources.volumeSizeGB : 0;
      volumeCount = resources.volumeCount;
      if (resources && resources.numNodes && renderType === 'Display') {
        universeNodes = (
          <YBResourceCount size={resources.numNodes || 0} kind="Node" pluralizeKind />
        );
      }
    }

    return this.props.split ? (
      this.props.split === 'left' ? (
        <div className={empty ? 'universe-resources empty' : 'universe-resources'}>
          {universeNodes}
          <YBResourceCount size={numCores || 0} kind="Core" pluralizeKind />
          <YBResourceCount size={memSizeGB || 0} unit="GB" kind="Memory" />
          <YBResourceCount size={volumeSizeGB || 0} unit="GB" kind="Storage" />
          <YBResourceCount size={volumeCount || 0} kind="Volume" pluralizeKind />
          {this.props.children}
        </div>
      ) : (
        <div className={empty ? 'universe-resources empty' : 'universe-resources'}>
          {renderCosts && (
            <>
              <YBResourceCount size={costPerDay} kind="/day" />
              <YBResourceCount size={costPerMonth} kind="/month" />
            </>
          )}
        </div>
      )
    ) : (
      <FlexContainer>
        <FlexGrow>
          <div className={empty ? 'universe-resources empty' : 'universe-resources'}>
            {universeNodes}
            <YBResourceCount size={numCores || 0} kind="Core" pluralizeKind />
            <YBResourceCount size={memSizeGB || 0} unit="GB" kind="Memory" />
            <YBResourceCount size={volumeSizeGB || 0} unit="GB" kind="Storage" />
            <YBResourceCount size={volumeCount || 0} kind="Volume" pluralizeKind />
            {renderCosts && (
              <>
                <YBResourceCount className="hidden-costs" size={costPerDay} kind="/day" />
                <YBResourceCount className="hidden-costs" size={costPerMonth} kind="/month" />
              </>
            )}
          </div>
        </FlexGrow>
        <FlexShrink className="universe-resource-btn-container">{this.props.children}</FlexShrink>
      </FlexContainer>
    );
  }
}
