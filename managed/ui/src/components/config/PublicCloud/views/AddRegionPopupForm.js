// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { connect } from 'react-redux';
import { reduxForm } from 'redux-form';
import { YBModal } from '../../../../components/common/forms/fields';

class AddRegionPopupForm extends Component {
  render() {
    const { handleSubmit } = this.props;
    return (
      <YBModal
        visible={this.props.visible}
        formName={'addRegionConfig'}
        onHide={this.props.onHide}
        submitLabel={this.props.submitLabel}
        showCancelButton={this.props.showCancelButton}
        title={this.props.title}
        onFormSubmit={handleSubmit(this.props.onFormSubmit)}
      >
        {this.props.children}
      </YBModal>
    );
  }
}

const mapStateToProps = (state, ownProps) => {
  if (ownProps.editRegion !== undefined) {
    return {
      initialValues: ownProps.editRegion
    };
  }
  return {};
};

export default connect(
  mapStateToProps,
  null
)(
  reduxForm({
    form: 'addRegionConfig'
  })(AddRegionPopupForm)
);
