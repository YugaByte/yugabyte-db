// Copyright YugaByte Inc.

import { ResourceStringPanel } from '../../panels';
import { connect } from 'react-redux';

function mapStateToProps(state) {
  return {
    providers: state.cloud.providers
  };
}

export default connect(mapStateToProps)(ResourceStringPanel);
