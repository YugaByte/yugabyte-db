import React, { FC } from 'react';
import { BaseExpandableSection, BaseExpandableSectionProps } from './BaseExpandableSection';
import { I18n } from '../../../../uikit/I18n/I18n';

type AdvancedSettingsProps = Pick<BaseExpandableSectionProps, 'expanded'>;

export const AdvancedSettings: FC<AdvancedSettingsProps> = ({ expanded, children }) => (
  <BaseExpandableSection title={<I18n>Advanced Settings</I18n>} expanded={expanded}>
    {children}
  </BaseExpandableSection>
);
