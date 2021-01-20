// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { BulkImport } from '../';
import { bulkImport, bulkImportResponse } from '../../../actions/tables';
import { reduxForm } from 'redux-form';
import _ from 'lodash';
import { isDefinedNotNull } from '../../../utils/ObjectUtils';

const mapDispatchToProps = (dispatch) => {
  return {
    bulkImport: (universeUUID, tableUUID, payload) => {
      dispatch(bulkImport(universeUUID, tableUUID, payload)).then((response) => {
        dispatch(bulkImportResponse(response.payload));
      });
    }
  };
};

function mapStateToProps(state) {
  return {
    universeDetails: state.universe.currentUniverse.data.universeDetails
  };
}

function validate(values) {
  const errors = {};
  let hasErrors = false;
  // s3Bucket is not optional and must be formatted as s3://<path>
  if (!values.s3Bucket) {
    errors.s3Bucket = 'S3 bucket path required.';
    hasErrors = true;
  } else if (!values.s3Bucket.startsWith('s3://')) {
    errors.s3Bucket = 'S3 bucket path must start with "s3://".';
    hasErrors = true;
  }
  // instanceCount is optional, but must be a positive non-zero number
  if (!isDefinedNotNull(values.instanceCount)) {
    if (!_.isNumber(values.instanceCount)) {
      errors.instanceCount = 'Invalid value. Must be a positive non-zero number.';
      hasErrors = true;
    } else if (parseInt(values.instanceCount, 10) < 1) {
      errors.instanceCount = 'Must have at least 1 task instance for EMR job.';
      hasErrors = true;
    }
  }
  return hasErrors && errors;
}

const bulkImportForm = reduxForm({
  form: 'BulkImport',
  fields: ['s3Bucket', 'instanceCount'],
  validate
});

export default connect(mapStateToProps, mapDispatchToProps)(bulkImportForm(BulkImport));
