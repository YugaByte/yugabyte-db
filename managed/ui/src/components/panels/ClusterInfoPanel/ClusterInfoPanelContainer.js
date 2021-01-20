// Copyright YugaByte Inc.

import { ClusterInfoPanel } from '../../panels';
import { connect } from 'react-redux';

function mapStateToProps(state) {
  return {
    providers: state.cloud.providers,
    insecure: !!state.customer.INSECURE_apiToken
  };
}

export default connect(mapStateToProps)(ClusterInfoPanel);
