// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';

import { CustomerMetricsPanel } from '../../metrics';

const mapStateToProps = (state) => {
  return {
    customer: state.customer
  };
};

export default connect(mapStateToProps)(CustomerMetricsPanel);
