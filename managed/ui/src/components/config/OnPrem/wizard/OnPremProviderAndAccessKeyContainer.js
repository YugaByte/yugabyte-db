// Copyright (c) YugaByte, Inc.

import { connect } from 'react-redux';
import { reduxForm } from 'redux-form';
import {OnPremProviderAndAccessKey} from '../../../config';
import {setOnPremConfigData} from '../../../../actions/cloud';
import {isDefinedNotNull, isNonEmptyObject, isNonEmptyArray} from '../../../../utils/ObjectUtils';
import _ from 'lodash';

const mapDispatchToProps = (dispatch, ownProps) => {
  return {
    setOnPremProviderAndAccessKey: (formData) => {
      Object.keys(formData).forEach((key) => { if (typeof formData[key] === 'string' || formData[key] instanceof String) formData[key] = formData[key].trim(); });
      if (!ownProps.isEditProvider) {
        const formSubmitVals = {
          provider: {
            name: formData.name,
            config: { YB_HOME_DIR: formData.homeDir, USE_HOSTNAME: _.get(formData, "useHostnames", false).toString() }
          },
          key: {
            code: formData.name.toLowerCase().replace(/ /g, "-") + "-key",
            privateKeyContent: formData.privateKeyContent,
            sshUser: formData.sshUser,
            sshPort: formData.sshPort,
            passwordlessSudoAccess: formData.passwordlessSudoAccess,
            airGapInstall: formData.airGapInstall
          }
        };
        dispatch(setOnPremConfigData(formSubmitVals));
      }
      ownProps.nextPage();
    }
  };
};

const mapStateToProps = (state, ownProps) => {
  let initialFormValues = {
    sshPort: 54422,
    passwordlessSudoAccess: true,
    airGapInstall: false,
    useHostnames: false
  };
  const {cloud: {onPremJsonFormData}} = state;
  if (ownProps.isEditProvider && isNonEmptyObject(onPremJsonFormData)) {
    initialFormValues = {
      name: onPremJsonFormData.provider.name,
      keyCode: onPremJsonFormData.key.code,
      privateKeyContent: onPremJsonFormData.key.privateKeyContent,
      sshUser: onPremJsonFormData.key.sshUser,
      sshPort: onPremJsonFormData.key.sshPort,
      passwordlessSudoAccess: onPremJsonFormData.key.passwordlessSudoAccess,
      airGapInstall: onPremJsonFormData.key.airGapInstall,
      useHostnames: _.get(onPremJsonFormData, "provider.config.USE_HOSTNAME", "false") === "true",
      homeDir: _.get(onPremJsonFormData, "provider.config.YB_HOME_DIR", ""),
      machineTypeList : onPremJsonFormData.instanceTypes.map(function (item) {
        return {
          code: item.instanceTypeCode,
          numCores: item.numCores,
          memSizeGB: item.memSizeGB,
          volumeSizeGB: isNonEmptyArray(item.volumeDetailsList) ? item.volumeDetailsList[0].volumeSizeGB : 0,
          mountPath: isNonEmptyArray(item.volumeDetailsList) ?  item.volumeDetailsList.map(function(volItem) {
            return volItem.mountPath;
          }).join(", ") : "/"
        };
      }),
      regionsZonesList: onPremJsonFormData.regions.map(function(regionZoneItem) {
        return {code: regionZoneItem.code,
          location: Number(regionZoneItem.latitude) + ", " + Number(regionZoneItem.longitude),
          zones: regionZoneItem.zones.map(function(zoneItem){
            return zoneItem;
          }).join(", ")};
      })
    };
  }
  return {
    onPremJsonFormData: state.cloud.onPremJsonFormData,
    cloud: state.cloud,
    initialValues: initialFormValues
  };
};

const validate = values => {
  const errors = {};
  if (!isDefinedNotNull(values.name)) {
    errors.name = 'Required';
  }
  if (!isDefinedNotNull(values.sshUser)) {
    errors.sshUser = 'Required';
  }
  if (!isDefinedNotNull(values.privateKeyContent)) {
    errors.privateKeyContent = 'Required';
  }
  return errors;
};

const onPremProviderConfigForm = reduxForm({
  form: 'onPremConfigForm',
  fields: ['name', 'sshUser', 'sshPort', 'privateKeyContent', 'passwordlessSudoAccess', 'airGapInstall', 'useHostnames', 'homeDir'],
  validate,
  destroyOnUnmount: false,
  enableReinitialize: true,
  keepDirtyOnReinitialize: true
});

export default connect(mapStateToProps, mapDispatchToProps)(onPremProviderConfigForm(OnPremProviderAndAccessKey));
