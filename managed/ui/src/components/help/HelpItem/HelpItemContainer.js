// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import HelpItems from './HelpItems';

const mapDispatchToProps = (dispatch) => {
  return {};
};

function mapStateToProps(state, ownProps) {
  return {
    customer: state.customer
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(HelpItems);
