// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col } from 'react-bootstrap';
import { Highlighter } from '../../../../helpers/Highlighter';
import AceEditor from 'react-ace';
import { YBPanelItem } from '../../../panels';
import { YBButton } from '../../../common/forms/fields';
import sampleDataCenterConfig from '../../templates/SampleDataCenterConfig.json';

import 'ace-builds/src-noconflict/theme-github';
import 'highlight.js/styles/github-gist.css';

class ConfigFormTitle extends Component {
  render() {
    const { titleText, copyTextToForm } = this.props;
    return (
      <div className="sample-config-item-label">
        <Col lg={9} className="color-grey">
          {titleText}
        </Col>
        <Col lg={3} className="text-right">
          <YBButton
            btnIcon="fa fa-angle-double-right"
            btnText={'Use Template'}
            onClick={copyTextToForm}
          />
        </Col>
      </div>
    );
  }
}

export default class OnPremConfigJSON extends Component {
  constructor(props) {
    super(props);
    this.sampleJsonPretty = JSON.stringify(sampleDataCenterConfig, null, 2);
  }

  onChange = (newValue) => {
    this.props.updateConfigJsonVal(newValue);
  };

  copyTextToForm = () => {
    this.props.updateConfigJsonVal(this.sampleJsonPretty);
  };

  render() {
    // Using Inline Styles because AceEditor is an SVG component
    // https://developer.mozilla.org/en-US/docs/Web/SVG
    const editorStyle = {
      width: '100%'
    };
    const configTitle = <h3>Enter Datacenter Configuration JSON:</h3>;
    const { switchToWizardEntry, submitJson } = this.props;
    return (
      <div>
        <Row className="form-data-container">
          <Col lg={5} className="sample-config-item">
            <Row className="color-light-grey">
              <ConfigFormTitle
                text={this.sampleJsonPretty}
                titleText={'Example Datacenter Configuration'}
                copyTextToForm={this.copyTextToForm}
              />
            </Row>
            <div className="onprem-config__json">
              <Highlighter type="json" text={this.sampleJsonPretty} element="pre" />
            </div>
          </Col>
          <Col lg={5} id="sample-panel-item">
            <YBPanelItem
              header={configTitle}
              body={
                <AceEditor
                  theme="github"
                  mode="json"
                  onChange={this.onChange}
                  name="dc-config-val"
                  value={this.props.configJsonVal}
                  style={editorStyle}
                  editorProps={{ $blockScrolling: true }}
                  showPrintMargin={false}
                  wrapEnabled={true}
                />
              }
              hideToolBox={true}
            />
          </Col>
        </Row>
        <div>
          {switchToWizardEntry}
          <YBButton
            btnText={'Submit'}
            btnType={'submit'}
            btnClass={'btn btn-default save-btn pull-right'}
            onClick={submitJson}
          />
        </div>
      </div>
    );
  }
}
