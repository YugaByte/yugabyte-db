// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { getPromiseState } from '../../../utils/PromiseUtils';
import 'react-bootstrap-multiselect/css/bootstrap-multiselect.css';
import { browserHistory } from 'react-router';
import { YBModal, YBCheckBox, YBTextInput } from '../../common/forms/fields';
import { isEmptyObject } from '../../../utils/ObjectUtils';
import { getReadOnlyCluster } from '../../../utils/UniverseUtils';

export default class DeleteUniverse extends Component {
  constructor(props) {
    super(props);
    this.state = {
      isForceDelete: false,
      universeName: false
    };
  }

  toggleForceDelete = () => {
    this.setState({ isForceDelete: !this.state.isForceDelete });
  };

  onChangeUniverseName = (value) => {
    this.setState({ universeName: value });
  };

  closeDeleteModal = () => {
    this.props.onHide();
  };

  getModalBody = () => {
    const {
      body,
      universe: {
        currentUniverse: {
          data: { name }
        }
      }
    } = this.props;
    return (
      <div>
        {body}
        <br />
        <br />
        <label>Enter universe name to confirm delete:</label>
        <YBTextInput
          label="Confirm universe name:"
          placeHolder={name}
          input={{ onChange: this.onChangeUniverseName, onBlur: () => {} }}
        />
      </div>
    );
  };

  confirmDelete = () => {
    const {
      type,
      universe: {
        currentUniverse: { data }
      }
    } = this.props;
    this.props.onHide();
    if (type === 'primary') {
      this.props.submitDeleteUniverse(data.universeUUID, this.state.isForceDelete);
    } else {
      const cluster = getReadOnlyCluster(data.universeDetails.clusters);
      if (isEmptyObject(cluster)) return;
      this.props.submitDeleteReadReplica(cluster.uuid, data.universeUUID, this.state.isForceDelete);
    }
  };

  componentDidUpdate(prevProps) {
    if (
      getPromiseState(prevProps.universe.deleteUniverse).isLoading() &&
      getPromiseState(this.props.universe.deleteUniverse).isSuccess()
    ) {
      this.props.fetchUniverseMetadata();
      browserHistory.push('/universes');
    }
  }

  render() {
    const {
      visible,
      title,
      error,
      onHide,
      universe: {
        currentUniverse: {
          data: { name }
        }
      }
    } = this.props;
    return (
      <YBModal
        visible={visible}
        formName={'DeleteUniverseForm'}
        onHide={onHide}
        submitLabel={'Yes'}
        cancelLabel={'No'}
        showCancelButton={true}
        title={title + name}
        onFormSubmit={this.confirmDelete}
        error={error}
        footerAccessory={
          <YBCheckBox
            label={'Ignore Errors and Force Delete'}
            className="footer-accessory"
            input={{ checked: this.state.isForceDelete, onChange: this.toggleForceDelete }}
          />
        }
        asyncValidating={this.state.universeName !== name}
      >
        {this.getModalBody()}
      </YBModal>
    );
  }
}
