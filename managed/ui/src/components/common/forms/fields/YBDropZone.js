// Copyright (c) YugaByte, Inc.

import React, { Fragment, Component } from 'react';
import Dropzone from 'react-dropzone';
import { YBLabel } from '../../../../components/common/descriptors';

import './stylesheets/YBDropZone.scss';

export default class YBDropZone extends Component {
  onDrop = (file) => {
    this.props.input.onChange(file[0]);
  };
  render() {
    const {
      input,
      title,
      meta: { touched, error }
    } = this.props;
    return (
      <Fragment>
        <div
          className={`form-group file-upload form-alt ${touched && error ? 'has-error' : ''} ${
            input.value ? 'has-value' : ''
          }`}
        >
          <Dropzone className={this.props.className} name={this.props.name} onDrop={this.onDrop}>
            <p>{title}</p>
          </Dropzone>
          {touched && error && <span className="help-block standard-error">{error}</span>}
          {input.value && <span className="drop-zone-file">{input.value.name}</span>}
        </div>
      </Fragment>
    );
  }
}

export class YBDropZoneWithLabel extends Component {
  onDrop = (file, e) => {
    this.props.input.onChange(file[0]);
  };
  render() {
    const {
      input,
      title,
      meta: { touched, error }
    } = this.props;
    const { insetError, infoContent, label, infoTitle, infoPlacement } = this.props;
    return (
      <YBLabel
        label={label}
        insetError={insetError}
        meta={this.props.meta}
        infoContent={infoContent}
        infoTitle={infoTitle}
        infoPlacement={infoPlacement}
      >
        <div
          className={`form-group yb-field-group form-alt file-upload ${
            touched && error ? 'has-error' : ''
          } ${input.value ? 'has-value' : ''}`}
        >
          <Dropzone className={this.props.className} name={this.props.name} onDrop={this.onDrop}>
            <p>{title}</p>
          </Dropzone>
          {touched && error && <span className="help-block standard-error">{error}</span>}
          {input.value && <span className="drop-zone-file">{input.value.name}</span>}
        </div>
      </YBLabel>
    );
  }
}
