// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import NavBarContainer from '../components/common/nav_bar/NavBarContainer';
import { Footer } from '../components/common/footer';
import AuthenticatedComponentContainer from '../components/Authenticated/AuthenticatedComponentContainer';
import { mouseTrap } from 'react-mousetrap';
import { browserHistory } from 'react-router';
import { YBModal, YBCheckBox } from '../components/common/forms/fields';
import { Table } from 'react-bootstrap';

import slackLogo from '../components/common/footer/images/slack-logo-full.svg';
import githubLogo from '../components/common/footer/images/github-light-small.png';
import tshirtImage from '../components/common/footer/images/tshirt-yb.png';
import ybLogo from '../components/common/YBLogo/images/yb_ybsymbol_dark.png';

class AuthenticatedComponent extends Component {
  constructor(props) {
    super(props);
    props.bindShortcut(
      [
        'ctrl+shift+n',
        'ctrl+shift+m',
        'ctrl+shift+t',
        'ctrl+shift+l',
        'ctrl+shift+c',
        'ctrl+shift+d'
      ],
      this._keyEvent
    );
    props.bindShortcut('?', this._toggleShortcutsHelp);
    this.state = {
      showKeyboardShortcuts: false,
      showIntroModal: false,
      hideDialogChecked: false
    };
  }

  componentDidMount() {
    const introState = localStorage.getItem('__yb_intro_dialog__');
    if (introState !== 'hidden' && introState !== 'existing') {
      this.setState({ showIntroModal: true });
    }
  }

  _toggleShortcutsHelp = () => {
    this.setState({ showKeyboardShortcuts: !this.state.showKeyboardShortcuts });
  };

  _keyEvent = (param) => {
    switch (param.key) {
      case 'N':
        browserHistory.push('/universes/create');
        break;
      case 'C':
        browserHistory.push('/config');
        break;
      case 'M':
        browserHistory.push('/metrics');
        break;
      case 'T':
        browserHistory.push('/tasks');
        break;
      case 'L':
        browserHistory.push('/universes');
        break;
      case 'D':
        browserHistory.push('/');
        break;
      default:
        break;
    }
  };

  closeIntroModal = () => {
    if (this.state.hideDialogChecked) {
      localStorage.setItem('__yb_intro_dialog__', 'hidden');
    } else {
      localStorage.setItem('__yb_intro_dialog__', 'existing');
    }
    this.setState({ showIntroModal: false });
  };

  render() {
    const { showKeyboardShortcuts, showIntroModal } = this.state;

    const introMessageStatus = (
      <div className="footer-accessory-wrapper">
        <YBCheckBox
          label={'Do not show this message in the future'}
          onClick={() => this.setState({ hideDialogChecked: true })}
        ></YBCheckBox>
      </div>
    );
    const welcomeDialogTitle = (
      <div>
        Welcome to 
        <img alt="YugaByte DB logo"
          className="social-media-logo"
          src={ybLogo}
          width="140"
          style={{verticalAlign: 'text-bottom'}}
        />
      </div>
    );
    return (
      <AuthenticatedComponentContainer>
        <NavBarContainer />
        <div className="container-body">
          {this.props.children}
          <YBModal
            title={'Keyboard Shortcut'}
            visible={showKeyboardShortcuts}
            onHide={() => this.setState({ showKeyboardShortcuts: false })}
          >
            <Table responsive>
              <thead>
                <tr>
                  <th>Shortcut</th>
                  <th>Description</th>
                </tr>
              </thead>
              <tbody>
                <tr>
                  <td>?</td>
                  <td>Show help</td>
                </tr>
                <tr>
                  <td>CTRL + SHIFT + n</td>
                  <td>Create new universe</td>
                </tr>
                <tr>
                  <td>CTRL + SHIFT + e</td>
                  <td>Edit universe</td>
                </tr>
                <tr>
                  <td>CTRL + SHIFT + c</td>
                  <td>Provider config</td>
                </tr>
                <tr>
                  <td>CTRL + SHIFT + m</td>
                  <td>View metrics</td>
                </tr>
                <tr>
                  <td>CTRL + SHIFT + t</td>
                  <td>View tasks</td>
                </tr>
                <tr>
                  <td>CTRL + SHIFT +l</td>
                  <td>View universes</td>
                </tr>
              </tbody>
            </Table>
          </YBModal>
          <YBModal
            title={welcomeDialogTitle}
            visible={showIntroModal}
            onHide={this.closeIntroModal}
            showCancelButton={true}
            cancelLabel={'Close'}
            footerAccessory={introMessageStatus}
          >
            <div className="intro-message-container">
              <a
                className="social-media-btn icon-end"
                href="https://www.yugabyte.com/slack"
                target="_blank"
                rel="noopener noreferrer"
              >
                <span>Join us on</span>
                <img alt="YugaByte DB Slack" src={slackLogo} width="65" />
              </a>
              <a
                className="social-media-btn icon-end"
                href="https://github.com/yugabyte/yugabyte-db"
                target="_blank"
                rel="noopener noreferrer"
              >
                <span>Star us on</span>
                <img
                  alt="YugaByte DB GitHub"
                  className="social-media-logo"
                  src={githubLogo}
                  width="18"
                />{' '}
                <b>GitHub</b>
              </a>
            </div>
            <div className="intro-message-container">
              <a
                className="social-media-btn"
                href="https://www.yugabyte.com/community-rewards"
                target="_blank"
                rel="noopener noreferrer"
              >
                <img alt="T-Shirt" src={tshirtImage} width="20" />
                <span>Get a Free t-shirt</span> 
              </a>
              <a
                className="social-media-btn"
                href="https://docs.yugabyte.com"
                target="_blank"
                rel="noopener noreferrer"
              >
                <i className="fa fa-search" />
                <span>Read docs</span>                
              </a>
            </div>
          </YBModal>
        </div>
        <Footer />
      </AuthenticatedComponentContainer>
    );
  }
}

export default mouseTrap(AuthenticatedComponent);
