import _ from 'lodash';
import { useUpdateEffect } from 'react-use';
import { browserHistory } from 'react-router';
import React, { createContext, FC, useReducer } from 'react';
import { WizardStep } from './compounds/WizardStepper/WizardStepper';
import { I18n } from '../../uikit/I18n/I18n';
import { CloudConfig, CloudConfigFormValue } from './steps/cloud/CloudConfig';
import { InstanceConfig, InstanceConfigFormValue } from './steps/instance/InstanceConfig';
import { DBConfig, DBConfigFormValue } from './steps/db/DBConfig';
import { SecurityConfig, SecurityConfigFormValue } from './steps/security/SecurityConfig';
import { HiddenConfig, Review } from './steps/review/Review';
import { Cluster, Universe } from '../../helpers/dtos';
import { PlacementUI } from './fields/PlacementsField/PlacementsField';
import { AUTH_GFLAG_YCQL, AUTH_GFLAG_YSQL } from '../dtoToFormData';
import './UniverseWizard.scss';

// To add a new form field:
// - add field to Universe type at dtos.ts and to corresponding form value config type (CloudConfigFormValue/InstanceConfigFormValue/...)
// - set default value in corresponding const at dtoToFormData.ts
// - populate form value from universe object in getFormData() at dtoToFormData.ts
// - map form value to payload in useLaunchUniverse() and maybe patchConfigResponse() at reviewHelpers.ts
// - add logic to "update_form_data" reducer case at UniverseWizard.tsx, if needed
// - add actual field ui component to corresponding step

// Field component requirements:
// - should update itself only and don't modify other form fields
// - could watch other form values
// - should have field validation logic, if needed

export enum ClusterOperation {
  NEW_PRIMARY,
  EDIT_PRIMARY,
  NEW_ASYNC,
  EDIT_ASYNC
}

export interface WizardStepsFormData {
  cloudConfig: CloudConfigFormValue;
  instanceConfig: InstanceConfigFormValue;
  dbConfig: DBConfigFormValue;
  securityConfig: SecurityConfigFormValue;
  hiddenConfig: HiddenConfig;
}

interface UniverseWizardProps {
  step: WizardStep;
  operation: ClusterOperation;
  formData: WizardStepsFormData;
  universe?: Universe; // parent universe
  cluster?: Cluster; // cluster to edit
}

interface WizardState {
  readonly isEditMode: boolean;
  readonly operation: ClusterOperation;
  readonly originalFormData: WizardStepsFormData;
  readonly universe?: Universe; // parent universe
  // readonly cluster?: Cluster; // TODO: remove if not needed anywhere
  formData: WizardStepsFormData;
  needExitWizard: boolean;
  activeStep: WizardStep;
}

export const WizardContext = createContext({} as WizardState);

export type WizardAction =
  | { type: 'change_step'; payload: WizardStep }
  | { type: 'exit_wizard'; payload: boolean }
  | { type: 'update_form_data'; payload: Partial<WizardStepsFormData> };

const reducer = (state: WizardState, action: WizardAction): WizardState => {
  switch (action.type) {
    case 'exit_wizard':
      // TODO: show modal with warning + reset flag on steps navigation
      return { ...state, needExitWizard: action.payload };
    case 'change_step':
      return {
        ...state,
        activeStep: action.payload
      };
    case 'update_form_data':
      const newFormData: WizardStepsFormData = { ...state.formData, ...action.payload };

      // if provider have changed - reset provider-dependent fields/configs
      if (newFormData.cloudConfig.provider?.uuid !== state.formData.cloudConfig.provider?.uuid) {
        newFormData.instanceConfig = _.cloneDeep(state.originalFormData.instanceConfig);
        newFormData.hiddenConfig.accessKeyCode = null;
      }

      // if placements have changed - update leads as per new placements
      if (!_.isEqual(newFormData.cloudConfig.placements, state.formData.cloudConfig.placements)) {
        newFormData.dbConfig.preferredLeaders = _.filter(
          _.compact<NonNullable<PlacementUI>>(newFormData.cloudConfig.placements),
          { isAffinitized: true }
        );
      }

      // userAZSelected is used for edit mode to indicate that something has changed with the placement comparing to original
      newFormData.hiddenConfig.userAZSelected =
        state.isEditMode &&
        !_.isEqual(
          newFormData.cloudConfig.placements,
          state.originalFormData.cloudConfig.placements
        );

      // add/remove gflags responsible for enable authentication feature toggle
      if (newFormData.securityConfig.enableAuthentication) {
        newFormData.dbConfig.tserverGFlags[AUTH_GFLAG_YSQL] = 'true';
        newFormData.dbConfig.tserverGFlags[AUTH_GFLAG_YCQL] = 'true';
      } else {
        newFormData.dbConfig.tserverGFlags = _.omit(newFormData.dbConfig.tserverGFlags, [
          AUTH_GFLAG_YSQL,
          AUTH_GFLAG_YCQL
        ]);
      }

      // reset root certificate if both node-to-node and client-to-node toggles are off
      if (
        !newFormData.securityConfig.enableNodeToNodeEncrypt &&
        !newFormData.securityConfig.enableClientToNodeEncrypt
      ) {
        newFormData.securityConfig.rootCA = null;
      }

      // reset key management service config if encryption at-rest toggle is off
      if (!newFormData.securityConfig.enableEncryptionAtRest) {
        newFormData.securityConfig.kmsConfig = null;
      }

      return {
        ...state,
        formData: newFormData
      };
  }
};

export const UniverseWizard: FC<UniverseWizardProps> = ({
  step,
  operation,
  formData: originalFormData,
  universe,
  cluster
}) => {
  const [wizardState, dispatch] = useReducer(reducer, {
    isEditMode:
      operation === ClusterOperation.EDIT_PRIMARY || operation === ClusterOperation.EDIT_ASYNC,
    operation,
    originalFormData,
    universe,
    // cluster,
    formData: originalFormData,
    needExitWizard: false,
    activeStep: step
  });

  useUpdateEffect(() => {
    if (wizardState.needExitWizard) {
      browserHistory.push('/');
    }
  }, [wizardState.needExitWizard]);

  return (
    <WizardContext.Provider value={wizardState}>
      <div className="universe-wizard">
        <div className="universe-wizard__header">
          <div className="universe-wizard__title">
            {operation === ClusterOperation.NEW_PRIMARY && <I18n>Create Universe</I18n>}
            {operation === ClusterOperation.NEW_ASYNC && <I18n>Create Read Replica</I18n>}
            {operation === ClusterOperation.EDIT_PRIMARY && <I18n>Edit Universe</I18n>}
            {operation === ClusterOperation.EDIT_ASYNC && <I18n>Edit Read Replica</I18n>}
            {wizardState.formData.cloudConfig.universeName && `: ${wizardState.formData.cloudConfig.universeName}` }
          </div>
          <div className="universe-wizard__beta">BETA</div>
        </div>

        <div className="universe-wizard__container">
          {wizardState.activeStep === WizardStep.Cloud && <CloudConfig dispatch={dispatch} />}
          {wizardState.activeStep === WizardStep.Instance && <InstanceConfig dispatch={dispatch} />}
          {wizardState.activeStep === WizardStep.Db && <DBConfig dispatch={dispatch} />}
          {wizardState.activeStep === WizardStep.Security && <SecurityConfig dispatch={dispatch} />}
          {wizardState.activeStep === WizardStep.Review && <Review dispatch={dispatch} />}
        </div>
      </div>
    </WizardContext.Provider>
  );
};
