// Copyright (c) YugaByte, Inc.

import React, { Fragment, Component } from 'react';
import Dropzone from 'react-dropzone';
import _ from 'lodash';

import './stylesheets/YBDropZone.scss';

export default class YBFormDropZone extends Component {
  onDrop = (acceptedFiles, e) => {
    const { name } = this.props.field;
    const { setFieldValue, setFieldTouched } = this.props.form;
    if (acceptedFiles.length === 0) {
      return;
    }
    setFieldValue(name, acceptedFiles[0]);
    setFieldTouched(name, true);
  };

  render() {
    const {
      title,
      field: { name },
      form,
      accept
    } = this.props;
    const { errors, values, touched } = form;
    const error = _.get(errors, name);
    const value = _.get(values, name);
    const hasError = error && (_.get(touched, name) || form.submitCount > 0);
    return (
      <Fragment>
        <div
          className={`form-group yb-field-group file-upload ${hasError ? 'has-error' : ''} ${
            value ? 'has-value' : ''
          }`}
        >
          <Dropzone
            className={this.props.className}
            name={name}
            accept={accept}
            onDrop={this.onDrop}
          >
            {title && <p>{title}</p>}
          </Dropzone>
          {hasError && <span className="help-block standard-error">{error}</span>}
          {value && <span className="drop-zone-file">{value.name}</span>}
        </div>
      </Fragment>
    );
  }
}
