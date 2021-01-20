// Copyright (c) YugaByte, Inc.

import React, { PureComponent } from 'react';
import './stylesheets/Footer.scss';
import slackLogo from './images/slack-logo-full.svg';
import githubLogo from './images/github-light-small.png';
import ybLogoImage from '../YBLogo/images/yb_ybsymbol_dark.png';
import YBLogo from '../YBLogo/YBLogo';
import * as moment from 'moment';
import { getPromiseState } from '../../../utils/PromiseUtils';

class Footer extends PureComponent {
  render() {
    const {
      customer: { yugawareVersion }
    } = this.props;
    const version = getPromiseState(yugawareVersion).isSuccess()
      ? yugawareVersion.data.version
      : null;
    return (
      <footer>
        <div className="flex-grow footer-logo-container">
          <YBLogo type="monochrome" />
          {version && <span> Version: {version.substr(0, version.indexOf('-'))}</span>}
        </div>
        <div className="flex-grow footer-social-container">
          <span className="social-media-cta">
            Join us on
            <a href="https://www.yugabyte.com/slack" target="_blank" rel="noopener noreferrer">
              <img alt="YugabyteDB Slack" src={slackLogo} width="65" />
            </a>
          </span>
          <span className="social-media-cta">
            Star us on
            <a
              href="https://github.com/yugabyte/yugabyte-db/"
              target="_blank"
              rel="noopener noreferrer"
            >
              <img
                alt="YugabyteDB GitHub"
                className="social-media-logo"
                src={githubLogo}
                width="18"
              />{' '}
              <b>GitHub</b>
            </a>
          </span>
          <span className="social-media-cta">
            Free t-shirt at
            <a
              href="https://www.yugabyte.com/community-rewards/"
              target="_blank"
              rel="noopener noreferrer"
            >
              <img
                alt="YugabyteDB Community Rewards"
                className="social-media-logo"
                src={ybLogoImage}
                width="100"
              />
            </a>
          </span>
        </div>
        <div className="flex-grow copyright">&copy; {moment().get('year')} Yugabyte, Inc.</div>
      </footer>
    );
  }
}

export default Footer;
