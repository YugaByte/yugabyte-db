import React, { Component } from 'react';
import { LinkContainer } from 'react-router-bootstrap';
import { MenuItem } from 'react-bootstrap';
import { isEnabled, isHidden } from '../../../../utils/LayoutUtils';

export class YBMenuItem extends Component {
  render() {
    const { availability, to, id, className, onClick } = this.props;
    if (isHidden(availability) && availability !== undefined) return null;
    if (isEnabled(availability)) {
      if (to) {
        return (
          <LinkContainer to={to} id={id}>
            <MenuItem className={className} onClick={onClick}>
              {this.props.children}
            </MenuItem>
          </LinkContainer>
        );
      } else {
        return (
          <MenuItem className={className} onClick={onClick}>
            {this.props.children}
          </MenuItem>
        );
      }
    }
    return (
      <li className={availability}>
        <div className={className}>{this.props.children}</div>
      </li>
    );
  }
}
