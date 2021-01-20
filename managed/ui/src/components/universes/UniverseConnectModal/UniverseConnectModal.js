// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import axios from 'axios';
import 'react-bootstrap-table/css/react-bootstrap-table.css';
import { YBModal, YBButton } from '../../common/forms/fields';
import { getUniverseEndpoint } from '../../../actions/common';
import { connect } from 'react-redux';
import { openDialog, closeDialog } from '../../../actions/modal';
import { FlexContainer, FlexShrink } from '../../common/flexbox/YBFlexBox';
import { getPromiseState } from '../../../utils/PromiseUtils';
import { isNonEmptyObject } from '../../../utils/ObjectUtils';
import { YBLoading } from '../../common/indicators';
import { YBCodeBlock, YBCopyButton } from '../../common/descriptors';
import { getPrimaryCluster } from '../../../utils/UniverseUtils';
import { isEnabled } from '../../../utils/LayoutUtils';
import _ from 'lodash';

import './UniverseConnectModal.scss';

class UniverseConnectModal extends Component {
  constructor(props) {
    super(props);
    this.state = {
      connectIp: null,
      endpointName: null,
      endpointPayload: '',
      endpointError: ''
    };
  }

  renderEndpointUrl = (endpointUrl, endpointName) => {
    return (
      <YBButton
        btnClass={`btn btn-default ${this.state.endpointName === endpointName && 'active'}`}
        onClick={this.handleEndpointCall.bind(this, endpointUrl, endpointName)}
        btnText={endpointName}
      />
    );
  };

  handleEndpointCall = (endpointUrl, endpointName) => {
    axios
      .get(endpointUrl)
      .then((response) =>
        this.setState({
          endpointPayload: response.data,
          endpointName: endpointName,
          endpointError: ''
        })
      )
      .catch(() =>
        this.setState({
          endpointPayload: '',
          endpointName: endpointName,
          endpointError: endpointName + ' endpoint is unavailable'
        })
      );
  };

  componentDidMount = () => {
    const {
      universe: { currentUniverse }
    } = this.props;
    if (getPromiseState(currentUniverse).isSuccess() && isNonEmptyObject(currentUniverse.data)) {
      const universeInfo = currentUniverse.data;
      const universeUUID = universeInfo.universeUUID;

      const {
        universeDetails: { clusters }
      } = universeInfo;
      const primaryCluster = getPrimaryCluster(clusters);
      const userIntent = primaryCluster && primaryCluster.userIntent;

      const ycqlServiceUrl = getUniverseEndpoint(universeUUID) + '/ysqlservers';
      // check if there's a Hosted Zone
      if (userIntent.providerType === 'aws' && universeInfo.dnsName) {
        this.setState({
          connectIp:
            universeInfo.dnsName.lastIndexOf('.') === universeInfo.dnsName.length - 1
              ? universeInfo.dnsName.substr(0, universeInfo.dnsName.length - 1)
              : universeInfo.dnsName
        });
        axios
          .get(ycqlServiceUrl)
          .then((response) =>
            this.setState({
              endpointName: 'YSQL',
              endpointPayload: response.data
            })
          )
          .catch(() => console.log('YCQL endpoint is unavailable'));
      } else {
        // if no go to YCQL endpoint and fetch IPs
        axios
          .get(ycqlServiceUrl)
          .then((response) =>
            this.setState({
              connectIp: response.data.split(',')[0].trim().split(':')[0],
              endpointName: 'YSQL',
              endpointPayload: response.data
            })
          )
          .catch(() => console.log('YCQL endpoint is unavailable'));
      }
    }
  };

  render() {
    const {
      currentCustomer,
      showOverviewConnectModal,
      closeModal,
      modal: { showModal, visibleModal },
      universe: { currentUniverse }
    } = this.props;

    let content = null;
    if (getPromiseState(currentUniverse).isLoading() || getPromiseState(currentUniverse).isInit()) {
      content = <YBLoading />;
    }
    if (getPromiseState(currentUniverse).isSuccess() && isNonEmptyObject(currentUniverse.data)) {
      const universeInfo = currentUniverse.data;
      const {
        universeDetails: { clusters, communicationPorts }
      } = universeInfo;
      const primaryCluster = getPrimaryCluster(clusters);
      const userIntent = primaryCluster && primaryCluster.userIntent;
      const universeId = universeInfo.universeUUID;
      const ysqlRpcPort = _.get(communicationPorts, 'ysqlServerRpcPort', 5433);

      const ysqlServiceUrl = getUniverseEndpoint(universeId) + '/ysqlservers';
      const ycqlServiceUrl = getUniverseEndpoint(universeId) + '/yqlservers';
      const yedisServiceUrl = getUniverseEndpoint(universeId) + '/redisservers';
      const endpointsContent = (
        <Fragment>
          <FlexContainer className="btn-group-cnt endpoint-buttons">
            {(userIntent.enableYSQL ||
              isEnabled(currentCustomer.data.features, 'universe.defaultYSQL')) && (
              <FlexShrink>{this.renderEndpointUrl(ysqlServiceUrl, 'YSQL')}</FlexShrink>
            )}
            <FlexShrink>{this.renderEndpointUrl(ycqlServiceUrl, 'YCQL')}</FlexShrink>
            {(userIntent.enableYEDIS ||
              isEnabled(currentCustomer.data.features, 'universe.defaultYEDIS', 'disabled')) && (
              <FlexShrink>{this.renderEndpointUrl(yedisServiceUrl, 'YEDIS')}</FlexShrink>
            )}
          </FlexContainer>
          <YBCodeBlock
            className={'endpoint-output' + (this.state.endpointPayload === '' ? ' empty' : '')}
          >
            <YBCopyButton text={this.state.endpointPayload} />
            {this.state.endpointPayload} {this.state.endpointError}
          </YBCodeBlock>
        </Fragment>
      );
      const connectIp = this.state.connectIp || '127.0.0.1';
      content = (
        <Fragment>
          <h4>Services</h4>
          <YBCodeBlock>
            <table>
              <tbody>
                <tr>
                  <td>JDBC</td>
                  <td>:</td>
                  <td title={`jdbc:postgresql://${connectIp}:${ysqlRpcPort}/yugabyte`}>
                    jdbc:postgresql://{connectIp}:{ysqlRpcPort}/yugabyte
                  </td>
                </tr>
                {(userIntent.enableYSQL ||
                  isEnabled(currentCustomer.data.features, 'universe.defaultYSQL')) && (
                  <tr>
                    <td>YSQL Shell</td>
                    <td>: </td>
                    <td>bin/ysqlsh</td>
                  </tr>
                )}
                <tr>
                  <td>YCQL Shell</td>
                  <td>: </td>
                  <td>bin/ycqlsh</td>
                </tr>
                {(userIntent.enableYEDIS ||
                  isEnabled(
                    currentCustomer.data.features,
                    'universe.defaultYEDIS',
                    'disabled'
                  )) && (
                  <tr>
                    <td>YEDIS Shell</td>
                    <td>: </td>
                    <td>bin/redis-cli</td>
                  </tr>
                )}
              </tbody>
            </table>
          </YBCodeBlock>
          <h4 className="endpoints-heading">Endpoints</h4>
          {endpointsContent}
        </Fragment>
      );
    }
    return (
      <Fragment>
        <YBButton
          btnText={'Connect'}
          btnClass={'btn btn-orange'}
          onClick={showOverviewConnectModal}
        />
        <YBModal
          title={'Connect'}
          visible={showModal && visibleModal === 'UniverseConnectModal'}
          onHide={closeModal}
          showCancelButton={true}
          cancelLabel={'Close'}
        >
          {content}
        </YBModal>
      </Fragment>
    );
  }
}

const mapDispatchToProps = (dispatch) => {
  return {
    showOverviewConnectModal: () => {
      dispatch(openDialog('UniverseConnectModal'));
    },
    closeModal: () => {
      dispatch(closeDialog());
    }
  };
};

function mapStateToProps(state) {
  return {
    currentCustomer: state.customer.currentCustomer,
    layout: state.customer.layout,
    universe: state.universe,
    modal: state.modal
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(UniverseConnectModal);
