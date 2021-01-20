// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';

import { StandaloneMetricsPanel } from '../../metrics';

const mapStateToProps = (state) => {
  return {
    graph: state.graph
  };
};

export default connect(mapStateToProps)(StandaloneMetricsPanel);
