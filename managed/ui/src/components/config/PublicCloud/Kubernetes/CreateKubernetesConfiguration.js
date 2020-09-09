// Copyright (c) YugaByte, Inc.

import React, {Component} from 'react';
import { Row, Col } from 'react-bootstrap';
import { YBButton } from '../../../common/forms/fields';
import { YBFormSelect, YBFormInput, YBFormDropZone } from '../../../common/forms/fields';
import YBInfoTip from '../../../common/descriptors/YBInfoTip';
import { isNonEmptyObject } from '../../../../utils/ObjectUtils';
import { readUploadedFile } from '../../../../utils/UniverseUtils';
import { KUBERNETES_PROVIDERS, REGION_DICT } from '../../../../config';
import { withRouter } from 'react-router';
import { Formik, Field } from 'formik';
import AddRegionList from './AddRegionList';
import * as Yup from "yup";
import JsYaml from "js-yaml";
import _ from 'lodash';

const convertStrToCode = s => s.trim().toLowerCase().replace(/\s/g, '-');

class CreateKubernetesConfiguration extends Component {
  createProviderConfig = (vals, setSubmitting) => {
    const { type } = this.props;

    const self = this;
    const pullSecretFile = vals.pullSecret;
    const providerName = vals.accountName;
    const readerSecret = readUploadedFile(pullSecretFile, false);
    const fileConfigArray = [readerSecret]; // Pull secret file is required
    // Record which regions and zones have configs with tuple of (region, zone)
    const configIndexRecord = [];
    const providerTypeMetadata = KUBERNETES_PROVIDERS.find(
      (providerType) => providerType.code === type
    );

    const providerKubeConfig = vals.kubeConfig ?
      readUploadedFile(vals.kubeConfig, false)
      : {};
    // Loop thru regions and check for config files
    vals.regionList.forEach((region, rIndex) => {
      region.zoneList.forEach((zone, zIndex) => {
        const content = zone.zoneKubeConfig;
        if (content) {
          if (!_.isEqual(content, vals.kubeConfig)){
            const zoneConfig = readUploadedFile(content, true);
            fileConfigArray.push(zoneConfig);
            configIndexRecord.push([rIndex, zIndex]);
          } else {
            fileConfigArray.push(providerKubeConfig);
            configIndexRecord.push([rIndex, zIndex]);
          }
        }
      });
    });

    // Loop thru regions to add location information
    const regionsLocInfo = vals.regionList.map(region => {
      const { code, latitude, longitude, name } = REGION_DICT[region.regionCode.value];
      return {
        name,
        code,
        latitude,
        longitude,
        zoneList: region.zoneList.map(zone => (
          {
            code: convertStrToCode(zone.zoneLabel),
            name: zone.zoneLabel,
            config: {
              STORAGE_CLASS: zone.storageClasses || 'standard',
              OVERRIDES: zone.zoneOverrides,
              KUBECONFIG_NAME: (zone.zoneKubeConfig && zone.zoneKubeConfig.name) || undefined,
            },
          }
        )),
      };
    });

    // Catch all onload events for configs
    Promise.all(fileConfigArray).then(configs => {
      const providerConfig = {
        KUBECONFIG_PROVIDER: vals.providerType ? vals.providerType.value :
            (providerTypeMetadata ? providerTypeMetadata.code : "gke"),
        KUBECONFIG_SERVICE_ACCOUNT: vals.serviceAccount,
        KUBECONFIG_IMAGE_REGISTRY: vals.imageRegistry || 'quay.io/yugabyte/yugabyte',
      };

      configIndexRecord.forEach(([regionIdx, zoneIdx], i) => {
        const currentZone = regionsLocInfo[regionIdx].zoneList[zoneIdx];
        currentZone.config.KUBECONFIG_CONTENT = configs[1 + i];
      });
      // TODO: fetch the service account name from the kubeconfig.

      if (isNonEmptyObject(pullSecretFile)) {
        const pullSecretYaml = JsYaml.safeLoad(configs[0]);
        Object.assign(providerConfig, {
          "KUBECONFIG_IMAGE_PULL_SECRET_NAME": pullSecretYaml.metadata && pullSecretYaml.metadata.name,
          "KUBECONFIG_PULL_SECRET_NAME": pullSecretFile.name,
          "KUBECONFIG_PULL_SECRET_CONTENT": configs[0]
        });
      }
      
      self.props.createKubernetesProvider(providerName.trim(), providerConfig, regionsLocInfo);
    }, reason => {
      console.warn("File Upload gone wrong. "+reason);
    });
    this.props.toggleListView(true);
    setSubmitting(false);
  }

  render() {
    const { type, modal, showModal, closeModal } = this.props;

    const providerTypeMetadata = KUBERNETES_PROVIDERS.find(
      (providerType) => providerType.code === type
    );
    let title = "Create Managed Kubernetes config";
    let providerTypeOptions = null;
    if (providerTypeMetadata) {
      providerTypeOptions = [
        {value: providerTypeMetadata.code, label: providerTypeMetadata.name }
      ];
      title = "Create " + providerTypeMetadata.name;
    } else {
      providerTypeOptions = KUBERNETES_PROVIDERS.map((provider) => {
        return {value: provider.code, label: provider.name};
      }).filter((p) => p.value !== "pks");
    }

    const initialValues = {
      providerType: null,
      accountName: "",
      serviceAccount: "",
      pullSecret: null,
      regionCode: "",
      zoneLabel: "",
      kubeConfig: null,
      imageRegistry: "",
      storageClasses: "",
      regionList: [],
      zoneOverrides: "",
    };

    Yup.addMethod(Yup.array, 'unique', function (message, mapper = a => a) {
      return this.test('unique', message, function (list) {
        return list.length === new Set(list.map(mapper)).size;
      });
    });

    const validationSchema = Yup.object().shape({
      accountName: Yup.string()
      .required('Config name is Required'),

      serviceAccount: Yup.string()
      .required('Service Account name is Required'),

      kubeConfig: Yup.mixed().nullable(),

      pullSecret: Yup.mixed(),

      regionCode: Yup.string(),

      regionList: Yup.array()
        .of(
          Yup.object().shape({
            regionCode: Yup.object()
              .nullable()
              .required('Region is required'),

            zoneList: Yup.array()
              .of(
                Yup.object().shape({
                  zoneLabel: Yup.string().required('Zone label is required'),
                }).required()
              )
              .unique('Duplicate zone label', a => a.zoneLabel),
          })
        ),
    });

    const containsValidRegion = (p) => {
      return p.values.regionList.filter(region => region.isValid).length > 0;
    };

    return (
      <div>
        <h2 className="table-container-title">{title}</h2>
        <div className="provider-config-container">
          <Formik
            validationSchema={validationSchema}
            initialValues={initialValues}
            onSubmit={(values, { setSubmitting }) => {
              const payload = {
                ...values,
              };
              this.createProviderConfig(payload, setSubmitting);
            }}
          >
            {props => (
              <form name="kubernetesConfigForm"
                    onSubmit={props.handleSubmit}>
                <div className="editor-container">
                  <Row>
                    <Col lg={8}>
                      <Row className="config-provider-row">
                        <Col lg={3}>
                          <div className="form-item-custom-label">Type</div>
                        </Col>
                        <Col lg={7}>
                          <Field name={"providerType"} component={YBFormSelect}
                                options={providerTypeOptions} />
                        </Col>
                      </Row>
                      <Row className="config-provider-row">
                        <Col lg={3}>
                          <div className="form-item-custom-label">Name</div>
                        </Col>
                        <Col lg={7}>
                          <Field name="accountName" placeholder="Kube Config name"
                                 component={YBFormInput}
                                 className={"kube-provider-input-field"}/>
                        </Col>
                      </Row>
                      <Row className="config-provider-row">
                        <Col lg={3}>
                          <div className="form-item-custom-label">Kube Config</div><div className={`help-block`}></div>
                        </Col>
                        <Col lg={7}>
                          <Field name="kubeConfig" component={YBFormDropZone}
                            className="upload-file-button"
                            title={"Upload Kube Config file"}/>
                        </Col>
                        <Col lg={1} className="config-provider-tooltip">
                          <YBInfoTip title="Kube Config"
                            content={"Use this setting to set a kube config for all regions and zones."} />
                        </Col>
                      </Row>
                      <Row className="config-provider-row">
                        <Col lg={3}>
                          <div className="form-item-custom-label">Service Account</div>
                        </Col>
                        <Col lg={7}>
                          <Field name="serviceAccount" placeholder="Service Account name"
                                 component={YBFormInput}
                                 className={"kube-provider-input-field"}/>
                        </Col>
                      </Row>
                      <Row className="config-provider-row">
                        <Col lg={3}>
                          <div className="form-item-custom-label">Image Registry</div>
                        </Col>
                        <Col lg={7}>
                          <Field name="imageRegistry" placeholder="quay.io/yugabyte/yugabyte"
                                 component={YBFormInput}
                                 className={"kube-provider-input-field"}/>
                        </Col>
                      </Row>
                      <Row className="config-provider-row">
                        <Col lg={3}>
                          <div className="form-item-custom-label">Pull Secret File</div>
                        </Col>
                        <Col lg={7}>
                          <Field name="pullSecret" component={YBFormDropZone}
                            className="upload-file-button"
                            title={"Upload Pull Secret file"}/>
                        </Col>
                      </Row>
                    </Col>
                  </Row>
                  <AddRegionList modal={modal} showModal={showModal} closeModal={closeModal} />
                </div>
                <div className="form-action-button-container">
                  <YBButton btnText={"Save"} disabled={props.isSubmitting || isNonEmptyObject(props.errors) || !containsValidRegion(props)}
                            btnClass={"btn btn-default save-btn"}
                            btnType="submit" />
                  {this.props.hasConfigs && <YBButton btnText={"Cancel"} disabled={props.isSubmitting}
                            btnClass={"btn btn-default cancel-btn"}
                            btnType="button" onClick={this.props.toggleListView} />}
                </div>
              </form>
            )}
          </Formik>
        </div>
      </div>
    );
  }
}

export default withRouter(CreateKubernetesConfiguration);
