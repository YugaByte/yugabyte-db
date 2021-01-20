import React, { FC } from 'react';
import { I18n, translate } from '../../../../uikit/I18n/I18n';
import { WizardStepsFormData } from '../../UniverseWizard';
import { useDeepCompareUpdateEffect } from '../../../../helpers/hooks';
import { CloudType } from '../../../../helpers/dtos';
import './Summary.scss';

const getProviderHumanName = (providerType: CloudType): string => {
  switch (providerType) {
    case CloudType.aws:
      return 'AWS';
    case CloudType.gcp:
      return 'GCP';
    case CloudType.azu:
      return 'Azure';
    case CloudType.docker:
      return 'Docker';
    case CloudType.onprem:
      return translate('On-Premises');
    case CloudType.kubernetes:
      return 'Kubernetes';
    case CloudType.cloud:
      return 'CloudOne';
    default:
      return translate('Other');
  }
};

interface SummaryProps {
  formData: WizardStepsFormData;
}

export const Summary: FC<SummaryProps> = ({ formData }) => {
  useDeepCompareUpdateEffect(() => {
    // console.log('formData changed', JSON.stringify(formData, null, 2)); // <-- TEMP DEBUG
    // TODO: trigger "/universe_resources" api on form data change
  }, [formData]);

  return (
    <div className="wizard-summary">
      <div className="wizard-summary__title">
        <I18n>Summary</I18n>
      </div>
      <div className="wizard-summary__content">
        {formData.cloudConfig.provider?.code && (
          <div className="wizard-summary__provider">
            {getProviderHumanName(formData.cloudConfig.provider.code)}
          </div>
        )}

        <div className="wizard-summary__number">0</div>
        <div className="wizard-summary__subtext">
          <I18n>Cores</I18n>
        </div>

        <div>
          <span className="wizard-summary__number">0</span>
          <span className="wizard-summary__subtext"> GB</span>
        </div>
        <div className="wizard-summary__subtext">
          <I18n>Memory</I18n>
        </div>

        <div>
          <span className="wizard-summary__number">0</span>
          <span className="wizard-summary__subtext"> GB</span>
        </div>
        <div className="wizard-summary__subtext">
          <I18n>Storage</I18n>
        </div>

        <div className="wizard-summary__number">0</div>
        <div className="wizard-summary__subtext">
          <I18n>Volumes</I18n>
        </div>

        <div className="wizard-summary__number">$0.00</div>
        <div className="wizard-summary__subtext">
          /<I18n>day</I18n>
        </div>

        <div className="wizard-summary__number">$0.00</div>
        <div className="wizard-summary__subtext">
          /<I18n>month</I18n>
        </div>
      </div>
    </div>
  );
};
