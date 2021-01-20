// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { reduxForm } from 'redux-form';
import { OnPremMachineTypes } from '../../../config';
import { setOnPremConfigData } from '../../../../actions/cloud';
import _ from 'lodash';
import { isNonEmptyArray, isDefinedNotNull } from '../../../../utils/ObjectUtils';

const mapDispatchToProps = (dispatch, ownProps) => {
  return {
    submitOnPremMachineTypes: (formData) => {
      const payloadObject = _.clone(ownProps.onPremJsonFormData);
      const instanceTypesList = formData.machineTypeList.map(function (item) {
        return {
          instanceTypeCode: item.code,
          numCores: item.numCores,
          memSizeGB: item.memSizeGB,
          volumeDetailsList: item.mountPath.split(',').map(function (mountPathItem) {
            return {
              volumeSizeGB: item.volumeSizeGB,
              volumeType: item.volumeType,
              mountPath: mountPathItem.trim()
            };
          }),
          volumeType: 'SSD',
          isBeingEdited: item.isBeingEdited
        };
      });
      payloadObject.instanceTypes = instanceTypesList;
      dispatch(setOnPremConfigData(payloadObject));
      ownProps.nextPage();
    }
  };
};

const mapStateToProps = (state) => {
  return {
    onPremJsonFormData: state.cloud.onPremJsonFormData
  };
};

const validate = (values) => {
  const errors = { machineTypeList: [] };
  if (values.machineTypeList && isNonEmptyArray(values.machineTypeList)) {
    values.machineTypeList.forEach(function (machineTypeItem, rowIdx) {
      if (!isDefinedNotNull(machineTypeItem.code)) {
        errors.machineTypeList[rowIdx] = { code: 'Required' };
      }
      if (!isDefinedNotNull(machineTypeItem.numCores)) {
        errors.machineTypeList[rowIdx] = { numCores: 'Required' };
      }
      if (!isDefinedNotNull(machineTypeItem.memSizeGB)) {
        errors.machineTypeList[rowIdx] = { memSizeGB: 'Required' };
      }
      if (!isDefinedNotNull(machineTypeItem.volumeSizeGB)) {
        errors.machineTypeList[rowIdx] = { volumeSizeGB: 'Required' };
      }
      if (
        isDefinedNotNull(machineTypeItem.volumeSizeGB) &&
        !/^\d+$/.test(machineTypeItem.volumeSizeGB)
      ) {
        errors.machineTypeList[rowIdx] = { volumeSizeGB: 'Must be Integer' };
      }
      if (isDefinedNotNull(machineTypeItem.numCores) && !/^\d+$/.test(machineTypeItem.numCores)) {
        errors.machineTypeList[rowIdx] = { numCores: 'Must be Integer' };
      }
      if (
        isDefinedNotNull(machineTypeItem.memSizeGB) &&
        !/(\d+(\.\d+)?)/.test(machineTypeItem.memSizeGB)
      ) {
        errors.machineTypeList[rowIdx] = { memSizeGB: 'Must be Numeric' };
      }
      if (!isDefinedNotNull(machineTypeItem.mountPath)) {
        errors.machineTypeList[rowIdx] = { mountPath: 'Required' };
      }
    });
  }
  return errors;
};

const onPremMachineTypesConfigForm = reduxForm({
  form: 'onPremConfigForm',
  validate,
  destroyOnUnmount: false
});

export default connect(
  mapStateToProps,
  mapDispatchToProps
)(onPremMachineTypesConfigForm(OnPremMachineTypes));
