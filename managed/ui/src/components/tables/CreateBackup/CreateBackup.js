// Copyright (c) YugaByte, Inc.

import React, { Component, Fragment } from 'react';
import PropTypes from 'prop-types';
import { components } from 'react-select';
import { browserHistory } from 'react-router';
import cronParser from 'cron-parser';
import moment from 'moment';
import { YBFormSelect, YBFormToggle, YBFormInput } from '../../common/forms/fields';
import YBInfoTip from '../../common/descriptors/YBInfoTip';
import { Row, Col, Tabs, Tab } from 'react-bootstrap';
import {
  isNonEmptyObject,
  isEmptyObject,
  isDefinedNotNull,
  isNonEmptyArray,
  isEmptyString,
  isNonEmptyString
} from '../../../utils/ObjectUtils';
import { Field } from 'formik';
import { YBModalForm } from '../../common/forms';
import _ from 'lodash';
import * as cron from 'cron-validator';

import '../common.scss';

const YSQL_TABLE_TYPE = 'PGSQL_TABLE_TYPE';
const YCQL_TABLE_TYPE = 'YQL_TABLE_TYPE';
const YEDIS_TABLE_TYPE = 'REDIS_TABLE_TYPE';

export default class CreateBackup extends Component {
  constructor(props) {
    super();
    let backupType = 'ysql';
    if (isNonEmptyObject(props.tableInfo)) {
      if (props.tableInfo.tableType === YCQL_TABLE_TYPE) {
        backupType = 'ycql';
      } else if (props.tableInfo.tableType === YEDIS_TABLE_TYPE) {
        backupType = 'yedis';
      }
    }
    this.state = {
      backupType
    };
  }

  static propTypes = {
    tableInfo: PropTypes.object
  };

  static getDerivedStateFromProps(props, state) {
    const { tableInfo } = props;
    if (isNonEmptyObject(tableInfo)) {
      if (tableInfo.tableType === YSQL_TABLE_TYPE) {
        return {
          backupType: 'ysql'
        };
      } else if (tableInfo.tableType === YCQL_TABLE_TYPE) {
        return {
          backupType: 'ycql'
        };
      } else if (tableInfo.tableType === YEDIS_TABLE_TYPE) {
        return {
          backupType: 'yedis'
        };
      }
    }
    return state;
  }

  createBackup = async (values) => {
    const {
      universeDetails: { universeUUID },
      onHide,
      onSubmit,
      onError,
      createTableBackup,
      createUniverseBackup,
      universeTables
    } = this.props;

    if (isDefinedNotNull(values.storageConfigUUID)) {
      let backupType = null;
      if (this.state.backupType === 'ysql') {
        backupType = YSQL_TABLE_TYPE;
      } else if (this.state.backupType === 'ycql') {
        backupType = YCQL_TABLE_TYPE;
      } else if (this.state.backupType === 'yedis') {
        backupType = YEDIS_TABLE_TYPE;
      }
      const payload = {
        storageConfigUUID: values.storageConfigUUID,
        sse: values.enableSSE,
        backupType: backupType,
        transactionalBackup: values.transactionalBackup,
        schedulingFrequency: isEmptyString(values.schedulingFrequency)
          ? null
          : values.schedulingFrequency,
        cronExpression: isNonEmptyString(values.cronExpression) ? values.cronExpression : null,
        parallelism: values.parallelism,
        timeBeforeDelete: values.timeBeforeDelete * 24 * 60 * 60 * 1000,
        actionType: 'CREATE'
      };
      try {
        let response = null;
        if (
          isDefinedNotNull(values.tableKeyspace) &&
          values.tableKeyspace.value === 'allkeyspaces'
        ) {
          // Backup all tables in all keyspaces
          response = await createUniverseBackup(universeUUID, payload);
        } else if (backupType === YSQL_TABLE_TYPE && isDefinedNotNull(values.tableKeyspace)) {
          payload.keyspace = values.tableKeyspace.value;
          response = await createUniverseBackup(universeUUID, payload);
        } else if (isDefinedNotNull(values.backupTableUUID)) {
          values.backupTableUUID = Array.isArray(values.backupTableUUID)
            ? values.backupTableUUID.map((x) => x.value)
            : [values.backupTableUUID.value];
          if (values.backupTableUUID[0] === 'alltables') {
            payload.keyspace = values.tableKeyspace.value;
            response = await createUniverseBackup(universeUUID, payload);
          } else if (values.backupTableUUID.length > 1) {
            payload.keyspace = values.tableKeyspace.value;
            payload.tableUUIDList = values.backupTableUUID;
            response = await createUniverseBackup(universeUUID, payload);
          } else {
            const backupTable = universeTables.find(
              (table) => table.tableUUID === values.backupTableUUID[0]
            );
            payload.tableName = backupTable.tableName;
            payload.keyspace = backupTable.keySpace;
            response = await createTableBackup(universeUUID, values.backupTableUUID[0], payload);
          }
        }
        onSubmit(response.data);
      } catch (err) {
        if (onError) {
          onError();
        }
      }
      onHide();
      browserHistory.push('/universes/' + universeUUID + '/backups');
    }
  };

  validateForm = (values) => {
    const errors = {};

    if (values.schedulingFrequency && !_.isNumber(values.schedulingFrequency)) {
      errors.schedulingFrequency = 'Frequency must be a number';
    }
    if (values.cronExpression && !cron.isValidCron(values.cronExpression)) {
      errors.cronExpression = 'Does not looks like a valid cron expression';
    }
    if (!values.storageConfigUUID || !('value' in values.storageConfigUUID)) {
      errors.storageConfigUUID = 'Storage Config is Required';
    }
    if (
      (values.schedulingFrequency || values.cronExpression) &&
      values.timeBeforeDelete != null &&
      values.timeBeforeDelete !== '' &&
      (!_.isNumber(values.timeBeforeDelete) || values.timeBeforeDelete < 0)
    ) {
      errors.timeBeforeDelete = 'Time before deletion needs to be in number of days';
    }

    if (values.parallelism === '' || values.parallelism == null) {
      errors.parallelism = 'Number of threads is required';
    } else if (!_.isNumber(values.parallelism)) {
      errors.parallelism = 'Parallelism must be a number';
    } else if (!Number.isInteger(values.parallelism)) {
      errors.parallelism = 'Value must be a whole number';
    } else if (values.parallelism < 1 || values.parallelism > 100) {
      errors.parallelism = 'Value must be between 1 and 100 inclusive';
    }

    if (!values.tableKeyspace || _.isEmpty(values.tableKeyspace)) {
      errors.tableKeyspace =
        this.state.backupType === 'ycql'
          ? 'Backup keyspace is required'
          : 'Backup namespace is required';
    }
    if (this.state.backupType === 'ycql') {
      if (
        !values.backupTableUUID ||
        (Array.isArray(values.backupTableUUID) && !values.backupTableUUID.length)
      ) {
        errors.backupTableUUID = 'Backup table is required';
      }
    }
    return errors;
  };

  backupKeyspaceChanged = (props, option) => {
    if (isNonEmptyObject(option) && !_.isEqual(option, props.field.value)) {
      props.form.setFieldValue('backupTableUUID', null);
    }
    props.form.setFieldValue(props.field.name, option);
  };

  backupTableChanged = (props, option) => {
    if (isNonEmptyObject(option) && option.value === 'alltables') {
      props.form.setFieldValue(props.field.name, option);
    } else if (isNonEmptyArray(option)) {
      const index = option.findIndex((item) => item.value === 'alltables');
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
  };

  render() {
    const { visible, isScheduled, onHide, tableInfo, storageConfigs, universeTables } = this.props;
    const { backupType } = this.state;
    const storageOptions = storageConfigs.map((config) => {
      return { value: config.configUUID, label: config.name + ' Storage' };
    });
    const initialValues = this.props.initialValues;

    let tableOptions = [];
    let keyspaceOptions = [];
    const keyspaces = new Set();
    let modalTitle = 'Create Backup';
    if (isNonEmptyObject(tableInfo)) {
      if (tableInfo.tableType === YSQL_TABLE_TYPE) {
        // YSQL does not do individual table backups
        initialValues.backupTableUUID = {
          label: <b>All Tables in Namespace</b>,
          value: 'alltables'
        };
      } else {
        tableOptions = [
          {
            value: tableInfo.tableID,
            label: tableInfo.keySpace + '.' + tableInfo.tableName,
            keyspace: tableInfo.keySpace
          }
        ];
        initialValues.backupTableUUID = tableOptions[0];
      }
      modalTitle = modalTitle + ' for ' + tableInfo.keySpace + '.' + tableInfo.tableName;
      initialValues.tableKeyspace = {
        label: tableInfo.keySpace,
        value: tableInfo.keySpace
      };
    } else {
      tableOptions = universeTables
        .map((tableInfo) => {
          keyspaces.add(tableInfo.keySpace);
          return {
            value: tableInfo.tableUUID,
            label: tableInfo.keySpace + '.' + tableInfo.tableName,
            keyspace: tableInfo.keySpace // Optional field for sorting
          };
        })
        .sort((a, b) => (a.label.toLowerCase() < b.label.toLowerCase() ? -1 : 1));
    }

    initialValues.schedulingFrequency = '';
    initialValues.cronExpression = '';

    const customOption = (props) => (
      <components.Option {...props}>
        <div className="input-select__option">
          {props.data.icon && <span className="input-select__option-icon">{props.data.icon}</span>}
          <span>{props.data.label}</span>
        </div>
      </components.Option>
    );

    const customSingleValue = (props) => (
      <components.SingleValue {...props}>
        {props.data.icon && (
          <span className="input-select__single-value-icon">{props.data.icon}</span>
        )}
        <span>{props.data.label}</span>
      </components.SingleValue>
    );

    return (
      <div className="universe-apps-modal">
        <YBModalForm
          title={modalTitle}
          visible={visible}
          onHide={onHide}
          showCancelButton={true}
          cancelLabel={'Cancel'}
          onFormSubmit={(values) => {
            const payload = {
              ...values,
              storageConfigUUID: values.storageConfigUUID.value
            };
            this.createBackup(payload);
          }}
          initialValues={initialValues}
          validate={this.validateForm}
          render={({
            values: {
              cronExpression,
              schedulingFrequency,
              storageConfigUUID,
              tableKeyspace,
              backupTableUUID
            },
            errors,
            setErrors,
            setFieldValue,
            setFieldTouched
          }) => {
            const isKeyspaceSelected = tableKeyspace?.value;
            const universeBackupSelected =
              isKeyspaceSelected && tableKeyspace.value === 'allkeyspaces';
            const isSchedulingFrequencyReadOnly = cronExpression !== '';
            const isCronExpressionReadOnly = schedulingFrequency !== '';
            const isTableSelected = backupTableUUID && backupTableUUID.length;
            const s3StorageSelected = storageConfigUUID && storageConfigUUID.label === 'S3 Storage';

            const showTransactionalToggle =
              isKeyspaceSelected &&
              !!isTableSelected &&
              (backupTableUUID.length > 1 || backupTableUUID[0].value === 'alltables');

            let displayedTables = [
              {
                label: <b>All Tables in Keyspace</b>,
                value: 'alltables'
              }
            ];

            if (!universeBackupSelected) {
              if (isKeyspaceSelected) {
                const filteredTables = tableOptions.filter(
                  (option) => option.keyspace === tableKeyspace.value
                );
                if (filteredTables.length) {
                  displayedTables.push({
                    label: 'Tables',
                    value: 'tables',
                    options: filteredTables
                  });
                } else {
                  displayedTables = [];
                }
              } else {
                displayedTables.push({
                  label: 'Tables',
                  value: 'tables',
                  options: tableOptions
                });
              }
            }

            const filteredKeyspaces = [...keyspaces]
              .filter((keyspace) => {
                if (backupType === 'ysql') {
                  return (
                    universeTables.find((x) => x.keySpace === keyspace).tableType ===
                    YSQL_TABLE_TYPE
                  );
                } else if (backupType === 'ycql') {
                  return (
                    universeTables.find((x) => x.keySpace === keyspace).tableType ===
                    YCQL_TABLE_TYPE
                  );
                } else if (backupType === 'yedis') {
                  return (
                    universeTables.find((x) => x.keySpace === keyspace).tableType ===
                    YEDIS_TABLE_TYPE
                  );
                }
                return false;
              })
              .map((key) => ({ value: key, label: key }));

            if (filteredKeyspaces.length) {
              keyspaceOptions = [
                {
                  label: <b>{backupType === 'ysql' ? 'All Namespaces' : 'All Keyspaces'}</b>,
                  value: 'allkeyspaces',
                  icon: <span className={'fa fa-globe'} />
                },
                {
                  label: backupType === 'ysql' ? 'Namespaces' : 'Keyspaces',
                  value: 'keyspaces',
                  options: filteredKeyspaces
                }
              ];
            }

            let nextCronExec = null;
            if (!isCronExpressionReadOnly && cronExpression && !errors.cronExpression) {
              try {
                const localDate = moment().utc();
                const iterator = cronParser.parseExpression(cronExpression, {
                  currentDate: localDate
                });
                nextCronExec = iterator.next().toDate().toString();
              } catch (e) {
                console.error('Invalid characters in cron expression');
              }
            }

            return (
              <Fragment>
                {isScheduled && (
                  <div className="backup-frequency-control">
                    <Row>
                      <Col xs={6}>
                        <Field
                          name="schedulingFrequency"
                          component={YBFormInput}
                          readOnly={isSchedulingFrequencyReadOnly}
                          type={'number'}
                          label={'Backup frequency'}
                          placeholder={'Interval in ms'}
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
                          placeholder={'Cron expression'}
                          label={'Cron expression (UTC)'}
                        />
                      </Col>
                      <Col lg={1} className="cron-expr-tooltip">
                        <YBInfoTip
                          title="Cron Expression Format"
                          content={
                            <div>
                              <code>Min&nbsp; Hour&nbsp; Day&nbsp; Mon&nbsp; Weekday</code>
                              <pre>
                                <code>* * * * * command to be executed</code>
                              </pre>
                              <pre>
                                <code>
                                  ┬ ┬ ┬ ┬ ┬<br />
                                  │ │ │ │ └─ Weekday (0=Sun .. 6=Sat)
                                  <br />
                                  │ │ │ └────── Month (1..12)
                                  <br />
                                  │ │ └─────────── Day (1..31)
                                  <br />
                                  │ └──────────────── Hour (0..23)
                                  <br />
                                  └───────────────────── Minute (0..59)
                                </code>
                              </pre>
                            </div>
                          }
                        />
                      </Col>
                    </Row>
                    {nextCronExec && (
                      <Row className="cron-description">
                        <Col lg={2}>Next job:</Col>
                        <Col>{nextCronExec}</Col>
                      </Row>
                    )}
                  </div>
                )}
                <Tabs
                  id="backup-api-tabs"
                  activeKey={backupType}
                  className="gflag-display-container"
                  onSelect={(k) => {
                    if (isEmptyObject(tableInfo) && k !== backupType) {
                      setFieldValue('tableKeyspace', null, false);
                      setFieldValue('backupTableUUID', null, false);
                      setFieldTouched('tableKeyspace', false);
                      setFieldTouched('backupTableUUID', false);
                      const newErrors = { ...errors };
                      delete newErrors.tableKeyspace;
                      delete newErrors.backupTableUUID;
                      setErrors(newErrors);
                      this.setState({ backupType: k });
                    }
                  }}
                >
                  <Tab
                    eventKey={'ysql'}
                    title="YSQL"
                    disabled={isNonEmptyObject(tableInfo) && backupType !== 'ysql'}
                  >
                    <Field
                      name="storageConfigUUID"
                      component={YBFormSelect}
                      label={'Storage'}
                      options={storageOptions}
                    />
                    <Field
                      name="tableKeyspace"
                      component={YBFormSelect}
                      components={{
                        Option: customOption,
                        SingleValue: customSingleValue
                      }}
                      label="Namespace"
                      options={keyspaceOptions}
                      onChange={this.backupKeyspaceChanged}
                      isDisabled={isNonEmptyObject(tableInfo)}
                    />
                    {s3StorageSelected && (
                      <Field name="enableSSE" component={YBFormToggle} label={'Encrypt Backup'} />
                    )}
                    <Field
                      name="parallelism"
                      component={YBFormInput}
                      type="number"
                      label={'Parallel Threads'}
                    />
                    {isScheduled && (
                      <Field
                        name="timeBeforeDelete"
                        type={'number'}
                        component={YBFormInput}
                        label={'Number of Days to Retain Backup'}
                      />
                    )}
                  </Tab>
                  <Tab
                    eventKey={'ycql'}
                    title="YCQL"
                    disabled={isNonEmptyObject(tableInfo) && backupType !== 'ycql'}
                  >
                    <Field
                      name="storageConfigUUID"
                      component={YBFormSelect}
                      label={'Storage'}
                      options={storageOptions}
                    />
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
                    {isKeyspaceSelected && (
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
                      </Row>
                    )}
                    {showTransactionalToggle && (
                      <Field
                        name="transactionalBackup"
                        component={YBFormToggle}
                        label={'Create a transactional backup across tables'}
                      />
                    )}
                    {s3StorageSelected && (
                      <Field name="enableSSE" component={YBFormToggle} label={'Encrypt Backup'} />
                    )}
                    <Field
                      name="parallelism"
                      component={YBFormInput}
                      type="number"
                      label={'Parallel Threads'}
                    />
                    {isScheduled && (
                      <Field
                        name="timeBeforeDelete"
                        type={'number'}
                        component={YBFormInput}
                        label={'Number of Days to Retain Backup'}
                      />
                    )}
                  </Tab>
                  <Tab
                    eventKey={'yedis'}
                    title="YEDIS"
                    disabled={isNonEmptyObject(tableInfo) && backupType !== 'yedis'}
                  >
                    <Field
                      name="storageConfigUUID"
                      component={YBFormSelect}
                      label={'Storage'}
                      options={storageOptions}
                    />
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
                    {isKeyspaceSelected && (
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
                      </Row>
                    )}
                    {showTransactionalToggle && (
                      <Field
                        name="transactionalBackup"
                        component={YBFormToggle}
                        label={'Create a transactional backup across tables'}
                      />
                    )}
                    {s3StorageSelected && (
                      <Field name="enableSSE" component={YBFormToggle} label={'Encrypt Backup'} />
                    )}
                    <Field
                      name="parallelism"
                      component={YBFormInput}
                      type="number"
                      label={'Parallel Threads'}
                    />
                    {isScheduled && (
                      <Field
                        name="timeBeforeDelete"
                        type="number"
                        component={YBFormInput}
                        label={'Number of Days to Retain Backup'}
                      />
                    )}
                  </Tab>
                </Tabs>
              </Fragment>
            );
          }}
        />
      </div>
    );
  }
}
