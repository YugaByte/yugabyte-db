// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import PropTypes from 'prop-types';
import { components } from 'react-select';
import { browserHistory } from 'react-router';
import cronParser from 'cron-parser';
import moment from 'moment';
import { YBFormSelect, YBFormToggle, YBFormInput } from '../../common/forms/fields';
import YBInfoTip from '../../common/descriptors/YBInfoTip';
import { Row, Col } from 'react-bootstrap';
import {
  isNonEmptyObject,
  isDefinedNotNull,
  isNonEmptyArray,
  isEmptyString,
  isNonEmptyString
} from '../../../utils/ObjectUtils';
import { Field } from 'formik';
import { YBModalForm } from '../../common/forms';
import _ from 'lodash';
import * as cron from 'cron-validator';
import * as Yup from "yup";

import '../common.scss';

const YSQL_TABLE_TYPE = 'PGSQL_TABLE_TYPE';
const schemaValidation =  Yup.object().shape({
  backupTableUUID: Yup.mixed().when('tableKeyspace', {
    is: (tableKeyspace) => !tableKeyspace || tableKeyspace.value === 'allkeyspaces',
    then: Yup.string().required('Backup keyspace and table are required').notOneOf([[]])
  }),
  tableKeyspace: Yup.object().required('Backup keyspace and table are required').notOneOf([[]]),
  storageConfigUUID: Yup.string().required('Storage Config is Required'),
  enableSSE: Yup.bool(),
  parallelism: Yup.number('Parallelism must be a number')
    .min(1)
    .max(100)
    .integer('Value must be a whole number')
    .required('Number of threads is required'),
  transactionalBackup: Yup.bool(),
  schedulingFrequency: Yup.number('Frequency must be a number'),
  cronExpression: Yup.string().test({
    name: "isValidCron",
    test: (value) => (value && cron.isValidCron(value)) || !value,
    message: 'Does not looks like a valid cron expression'
  })
});

export default class CreateBackup extends Component {
  static propTypes = {
    tableInfo: PropTypes.object
  };

  createBackup = values => {
    const {
      universeDetails: { universeUUID },
      onHide,
      createTableBackup,
      createUniverseBackup,
      universeTables
    } = this.props;

    if (isDefinedNotNull(values.storageConfigUUID)) {
      const payload = {
        "storageConfigUUID": values.storageConfigUUID,
        "sse": values.enableSSE,
        "transactionalBackup": values.transactionalBackup,
        "schedulingFrequency": isEmptyString(values.schedulingFrequency) ? null : values.schedulingFrequency,
        "cronExpression": isNonEmptyString(values.cronExpression) ? values.cronExpression : null,
        "parallelism": values.parallelism,
        "actionType": "CREATE"
      };
      if (isDefinedNotNull(values.tableKeyspace) && values.tableKeyspace.value === "allkeyspaces") {
        // Backup all tables in all keyspaces
        createUniverseBackup(universeUUID, payload);
      } else if (isDefinedNotNull(values.backupTableUUID)) {
        values.backupTableUUID = Array.isArray(values.backupTableUUID) ?
          values.backupTableUUID.map(x => x.value) : [values.backupTableUUID.value];
        if (values.backupTableUUID[0] === "alltables") {
          payload.keyspace = values.tableKeyspace.value;
          createUniverseBackup(universeUUID, payload);
        } else if (values.backupTableUUID.length > 1) {
          payload.keyspace = values.tableKeyspace.value;
          payload.tableUUIDList = values.backupTableUUID;
          createUniverseBackup(universeUUID, payload);
        } else {
          const backupTable = universeTables
            .find((table) => table.tableUUID === values.backupTableUUID[0]);
          payload.tableName = backupTable.tableName;
          payload.keyspace = backupTable.keySpace;
          createTableBackup(universeUUID, values.backupTableUUID[0], payload);
        }
      }
      onHide();
      browserHistory.push('/universes/' + universeUUID + "/backups");
    }
  };

  backupKeyspaceChanged = (props, option) => {
    if (isNonEmptyObject(option) && !_.isEqual(option, props.field.value)) {
      props.form.setFieldValue("backupTableUUID", null);
    }
    props.form.setFieldValue(props.field.name, option);
  }

  backupTableChanged = (props, option) => {
    if (isNonEmptyObject(option) && option.value === "alltables") {
      props.form.setFieldValue(props.field.name, option);
    } else if (isNonEmptyArray(option)) {
      const index = option.findIndex(item => item.value === "alltables");
      if (index > -1) {
        // Clear all other values except 'All Tables in Keyspace'
        props.form.setFieldValue(props.field.name, [option[index]]);
      } else {
        props.form.setFieldValue(props.field.name, option);
      }
    } else {
      // Clear form
      props.form.setFieldValue(props.field.name, []);
    }
  }

  render() {
    const { visible, isScheduled, onHide, tableInfo, storageConfigs, universeTables } = this.props;
    const storageOptions = storageConfigs.map((config) => {
      return {value: config.configUUID, label: config.name + " Storage"};
    });
    const initialValues = this.props.initialValues;

    let tableOptions = [];
    let keyspaceOptions = [];
    const keyspaces = new Set();
    let modalTitle = "Create Backup";
    if (isNonEmptyObject(tableInfo)) {
      if (tableInfo.tableType === YSQL_TABLE_TYPE) {
        // YSQL does not do individual table backups
        initialValues.backupTableUUID = {
          label: <b>All Tables in Keyspace</b>,
          value: "alltables",
        };
      } else {
        tableOptions = [{
          value: tableInfo.tableID,
          label: tableInfo.keySpace + "." + tableInfo.tableName,
          keyspace: tableInfo.keySpace
        }];
        initialValues.backupTableUUID = tableOptions[0];
      }
      modalTitle = modalTitle + " for " + tableInfo.keySpace + "." + tableInfo.tableName;
      initialValues.tableKeyspace = {
        label: tableInfo.keySpace,
        value: tableInfo.keySpace
      };
    } else {
      tableOptions = universeTables.map((tableInfo) => {
        keyspaces.add(tableInfo.keySpace);
        return {
          value: tableInfo.tableUUID,
          label: tableInfo.keySpace + "." + tableInfo.tableName,
          keyspace: tableInfo.keySpace // Optional field for sorting
        };
      }).sort((a, b) => a.label.toLowerCase() < b.label.toLowerCase() ? -1 : 1);
    }

    initialValues.schedulingFrequency = "";
    initialValues.cronExpression = "";

    const customOption = (props) => (<components.Option {...props}>
      <div className="input-select__option">
        { props.data.icon && <span className="input-select__option-icon">{ props.data.icon }</span> }
        <span>{ props.data.label }</span>
      </div>
    </components.Option>);

    const customSingleValue = (props) => (<components.SingleValue {...props}>
      { props.data.icon && <span className="input-select__single-value-icon">{ props.data.icon }</span> }
      <span>{ props.data.label }</span>
    </components.SingleValue>);

    return (
      <div className="universe-apps-modal">
        <YBModalForm
          title={modalTitle}
          visible={visible}
          onHide={onHide}
          showCancelButton={true}
          cancelLabel={"Cancel"}
          onFormSubmit={(values) => {
            const payload = {
              ...values,
              storageConfigUUID: values.storageConfigUUID.value,
            };
            this.createBackup(payload);
          }}
          initialValues={initialValues}
          validationSchema={schemaValidation}
          render={({
            values: { cronExpression, schedulingFrequency, backupTableUUID, storageConfigUUID, tableKeyspace, parallelism  },
            values,
            errors
          }) => {
            const isKeyspaceSelected = tableKeyspace && tableKeyspace.value;
            const universeBackupSelected = isKeyspaceSelected && tableKeyspace.value === 'allkeyspaces';
            const isYSQLKeyspace = isKeyspaceSelected && !universeBackupSelected &&
              universeTables.find(x => x.keySpace === values.tableKeyspace.value).tableType === YSQL_TABLE_TYPE;
            const isSchedulingFrequencyReadOnly = cronExpression !== "";
            const isCronExpressionReadOnly = schedulingFrequency !== "";
            const isTableSelected = backupTableUUID && backupTableUUID.length;

            const s3StorageSelected = storageConfigUUID && storageConfigUUID.label === 'S3 Storage';

            const showTransactionalToggle = isKeyspaceSelected &&
              (!!isTableSelected && (backupTableUUID.length > 1 || backupTableUUID[0].value === 'alltables'));

            const displayedTables = [
              {
                label: <b>All Tables in Keyspace</b>,
                value: "alltables",
              }
            ];

            if (!universeBackupSelected && !isYSQLKeyspace) {
              displayedTables.push({
                label: "Tables",
                value: 'tables',
                options: isKeyspaceSelected ?
                  tableOptions.filter(option => option.keyspace === tableKeyspace.value) :
                  tableOptions
              });
            }
            keyspaceOptions = [{
              label: <b>All Keyspaces</b>,
              value: "allkeyspaces",
              icon: <span className={"fa fa-globe"} />
            },
            {
              label: "Keyspaces",
              value: 'keyspaces',
              options: [...keyspaces].map(key => ({ value: key, label: key }))
            }];
            
            let nextCronExec = null;
            if (!isCronExpressionReadOnly && cronExpression && !errors.cronExpression) {
              try {
                const localDate = moment().utc();
                let iterator = cronParser.parseExpression(cronExpression, { currentDate: localDate })                
                nextCronExec = iterator.next().toDate().toString();
              } catch (e) {
                console.error('Invalid characters in cron expression');
              }
            }

            // params for backupTableUUID <Field>
            // NOTE: No entire keyspace selection implemented
            return (<Fragment>
              {isScheduled &&
                <div className="backup-frequency-control">
                  <Row>
                    <Col xs={6}>
                      <Field
                        name="schedulingFrequency"
                        component={YBFormInput}
                        readOnly={isSchedulingFrequencyReadOnly}
                        type={"number"}
                        label={"Backup frequency"}
                        placeholder={"Interval in ms"}
                      />
                    </Col>
                  </Row>
                  <div className="separating-text">OR</div>
                  <Row>
                    <Col xs={6}>
                      <Field
                        name="cronExpression"
                        component={YBFormInput}
                        readOnly={isCronExpressionReadOnly}
                        placeholder={"Cron expression"}
                        label={"Cron expression (UTC)"}
                      />
                    </Col>
                    <Col lg={1} className="cron-expr-tooltip">
                      <YBInfoTip title="Cron Expression Format"
                        content={<div><code>Min&nbsp; Hour&nbsp; Day&nbsp; Mon&nbsp; Weekday</code>
                          <pre><code>*    *    *    *    *  command to be executed</code></pre>
                          <pre><code>┬    ┬    ┬    ┬    ┬<br />
│    │    │    │    └─  Weekday  (0=Sun .. 6=Sat)<br />
│    │    │    └──────  Month    (1..12)<br />
│    │    └───────────  Day      (1..31)<br />
│    └────────────────  Hour     (0..23)<br />
└─────────────────────  Minute   (0..59)</code></pre>
                        </div>} />
                    </Col>
                  </Row>
                  {nextCronExec && 
                    <Row className="cron-description">
                      <Col lg={2}>Next job:</Col>
                      <Col>{nextCronExec}</Col>
                    </Row>
                  }
                </div>
              }
              <Field
                name="storageConfigUUID"
                component={YBFormSelect}
                label={"Storage"}
                options={storageOptions}
              />
              {!!keyspaceOptions.length &&
                <Field
                  name="tableKeyspace"
                  component={YBFormSelect}
                  components={{
                    Option: customOption,
                    SingleValue: customSingleValue
                  }}
                  label="Keyspace"
                  options={keyspaceOptions}
                  onChange={this.backupKeyspaceChanged}
                  isDisabled={isNonEmptyObject(tableInfo)}
                />
              }
              {isKeyspaceSelected &&
                <Row>
                  <Col xs={6}>
                    <Field
                      name="backupTableUUID"
                      component={YBFormSelect}
                      components={{
                        Option: customOption,
                        SingleValue: customSingleValue
                      }}
                      label={`Tables to backup`}
                      options={displayedTables}
                      isMulti={true}
                      onChange={this.backupTableChanged}
                      isDisabled={isNonEmptyObject(tableInfo)}
                    />
                  </Col>
                  {isYSQLKeyspace &&
                    <Col lg={1} className="config-zone-tooltip">
                      <YBInfoTip title="Backups in YSQL"
                        content={<div>Table-level backups are unavailable for YSQL.<br />Please use keyspace-level backups instead.</div>} />
                    </Col>
                  }
                </Row>
              }
              {showTransactionalToggle &&
                <Field
                  name="transactionalBackup"
                  component={YBFormToggle}
                  label={"Create a transactional backup across tables"}
                />
              }
              {s3StorageSelected && <Field
                name="enableSSE"
                component={YBFormToggle}
                label={"Encrypt Backup"}
              />
              }
              <Field
                name="parallelism"
                component={YBFormInput}
                label={"Parallel Threads"}
              />
            </Fragment>);
          }}
        />
      </div>
    );
  }
}
