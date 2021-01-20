// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';

import Footer from './Footer';

const mapDispatchToProps = (dispatch) => {
  return {};
};

const mapStateToProps = (state) => {
  return {
    customer: state.customer
  };
};

export default connect(mapStateToProps, mapDispatchToProps)(Footer);
