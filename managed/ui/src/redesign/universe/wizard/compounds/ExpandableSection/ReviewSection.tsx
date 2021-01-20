import React, { FC } from 'react';
import { BaseExpandableSection, BaseExpandableSectionProps } from './BaseExpandableSection';
import { I18n } from '../../../../uikit/I18n/I18n';

type ReviewSectionProps = Required<
  Pick<BaseExpandableSectionProps, 'title' | 'expanded' | 'onEditClick'>
>;

export const ReviewSection: FC<ReviewSectionProps> = ({
  title,
  expanded,
  onEditClick,
  children
}) => (
  <BaseExpandableSection
    title={title}
    fixedTitleWidth={155}
    expanded={expanded}
    showLine
    editLink={<I18n>Edit</I18n>}
    onEditClick={onEditClick}
  >
    {children}
  </BaseExpandableSection>
);
