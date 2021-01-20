import _ from 'lodash';
import clsx from 'clsx';
import React, { FC, ReactNode, useState } from 'react';
import { I18n } from '../../../../uikit/I18n/I18n';
import { useUpdateEffect } from 'react-use';
import './BaseExpandableSection.scss';

export interface BaseExpandableSectionProps {
  title: ReactNode;
  expanded: boolean;
  fixedTitleWidth?: number;
  showLine?: boolean;
  editLink?: ReactNode;
  onEditClick?: () => void;
}

export const BaseExpandableSection: FC<BaseExpandableSectionProps> = ({
  title,
  expanded,
  fixedTitleWidth,
  showLine,
  editLink,
  onEditClick,
  children
}) => {
  const [internalIsExpanded, setInternalIsExpanded] = useState<boolean>(expanded);

  useUpdateEffect(() => {
    setInternalIsExpanded(expanded);
  }, [expanded]);

  const toggleExpanded = () => setInternalIsExpanded(!internalIsExpanded);

  const noChildren = !children || (Array.isArray(children) && _.isEmpty(_.compact(children)));

  // skip rendering component if there are no children (i.e. no components in expandable area)
  if (noChildren) {
    return null;
  } else {
    return (
      <div className="wizard-expandable-section">
        <div className="wizard-expandable-section__head">
          <I18n
            className="wizard-expandable-section__title"
            style={{ width: fixedTitleWidth ? `${fixedTitleWidth}px` : 'auto' }}
          >
            {title}
          </I18n>
          <div className="wizard-expandable-section__plus-btn" onClick={toggleExpanded}>
            {internalIsExpanded ? '−' : '+'}
          </div>
          {showLine && <div className="wizard-expandable-section__line" />}
          {editLink && (
            <div className="wizard-expandable-section__edit-link" onClick={onEditClick}>
              {editLink}
            </div>
          )}
        </div>
        <div
          className={clsx('wizard-expandable-section__body', {
            'wizard-expandable-section__body--show': internalIsExpanded,
            'wizard-expandable-section__body--hide': !internalIsExpanded
          })}
        >
          {children}
        </div>
      </div>
    );
  }
};
