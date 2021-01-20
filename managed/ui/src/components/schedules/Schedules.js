import React, { Component, Fragment } from 'react';
import { YBPanelItem } from '../panels';
import { Row, Col, Alert, DropdownButton, MenuItem } from 'react-bootstrap';
import { getPromiseState } from '../../utils/PromiseUtils';
import moment from 'moment';
import _ from 'lodash';
import pluralize from 'pluralize';
import cronstrue from 'cronstrue';
import { DescriptionItem } from '../common/descriptors';
import { isNonEmptyObject, isDefinedNotNull, isNonEmptyArray } from '../../utils/ObjectUtils';
import { isAvailable, isNonAvailable } from '../../utils/LayoutUtils';
import { YBModalForm } from '../common/forms';
import { TableAction } from '../tables';

import './schedules.scss';

class ScheduleDisplayItem extends Component {
  handleDeleteMenuItemClick = (scheduleUUID) => {
    this.props.handleDeleteAction(scheduleUUID);
  };
  render() {
    const { name, idx, readOnly } = this.props;
    const schedule = _.cloneDeep(this.props.schedule);

    const getReadableTime = (msecs) => {
      const time = moment.duration(msecs);
      const timeArray = [
        [time.seconds(), 'sec'],
        [time.minutes(), 'min'],
        [time.hours(), 'hour'],
        [time.days(), 'day'],
        [time.months(), 'month'],
        [time.years(), 'year']
      ];

      const reducedTimeArray = timeArray.filter((item) => item[0] !== 0).slice(0, 2);
      return isDefinedNotNull(reducedTimeArray[1])
        ? `Every ${reducedTimeArray[1][0]}
           ${pluralize(reducedTimeArray[1][1], reducedTimeArray[1][0])}
           ${reducedTimeArray[0][0]}
           ${pluralize(reducedTimeArray[0][1], reducedTimeArray[0][0])}`
        : `Every ${reducedTimeArray[0][0]}
           ${pluralize(reducedTimeArray[0][1], reducedTimeArray[0][0])}`;
    };

    if (schedule.frequency) {
      schedule.frequency = getReadableTime(schedule.frequency);
    }
    if (schedule.cronExpression) {
      schedule.cronExpression = cronstrue.toString(schedule.cronExpression);
    }

    let taskType = '';
    let keyspace = '';
    let tableDetails = '';
    if (schedule.taskType === 'BackupUniverse') {
      taskType = 'Table Backup';
      tableDetails = `${schedule.taskParams.keyspace}.${schedule.taskParams.tableName}`;
    } else if (schedule.taskType === 'MultiTableBackup' && schedule.taskParams.keyspace) {
      taskType = 'Full Universe Backup';
      keyspace = schedule.taskParams.keyspace;
      tableDetails = 'All';
    } else if (schedule.taskType === 'MultiTableBackup') {
      taskType = 'Full Universe Backup';
      keyspace = 'All';
      tableDetails = 'All';
    } else {
      console.error(`Unknown task type: ${schedule.taskType}`);
      taskType = 'Unknown';
      keyspace = (schedule.taskParams && schedule.taskParams.keyspace) || 'N/A';
      tableDetails = (schedule.taskParams && schedule.taskParams.tableName) || 'N/A';
    }

    const retentionTime =
      !schedule.taskParams.timeBeforeDelete || schedule.taskParams.timeBeforeDelete === 0
        ? 'Unlimited'
        : moment.duration(schedule.taskParams.timeBeforeDelete).humanize();

    return (
      <Col xs={12} sm={6} md={6} lg={4}>
        <div className="schedule-display-item-container">
          {!readOnly && (
            <div className="status-icon">
              <DropdownButton
                bsStyle={'default'}
                title={<span className="fa fa-ellipsis-v" />}
                key={name + idx}
                noCaret
                pullRight
                id={`dropdown-basic-${name}-${idx}`}
              >
                <MenuItem
                  eventKey="1"
                  className="menu-item-delete"
                  onClick={this.handleDeleteMenuItemClick.bind(this, schedule.scheduleUUID)}
                >
                  <span className="fa fa-trash" />
                  Delete schedule
                </MenuItem>
              </DropdownButton>
            </div>
          )}
          <div className="display-name">{name}</div>
          <div className="provider-name">{taskType}</div>
          <div className="description-item-list">
            <DescriptionItem title={'Schedule (UTC)'}>
              <span>{schedule.frequency || schedule.cronExpression}</span>
            </DescriptionItem>
            <DescriptionItem title={'Encrypt Backup'}>
              <span>{schedule.taskParams.sse ? 'On' : 'Off'}</span>
            </DescriptionItem>
            <DescriptionItem title={'Keyspace'}>
              <span>{keyspace}</span>
            </DescriptionItem>
            <DescriptionItem title={'Table'}>
              <span>{tableDetails}</span>
            </DescriptionItem>
            <DescriptionItem title={'Retention Policy'}>
              <span>{retentionTime}</span>
            </DescriptionItem>
          </div>
        </div>
      </Col>
    );
  }
}

class Schedules extends Component {
  constructor(props) {
    super(props);
    this.state = {
      scheduleUUID: null
    };
  }

  handleDeleteAction = (scheduleUUID) => {
    const { showAddCertificateModal } = this.props;
    showAddCertificateModal();
    this.setState({ scheduleUUID });
  };

  handleDeleteSchedule = () => {
    const { deleteSchedule } = this.props;
    const scheduleUUID = this.state.scheduleUUID;
    deleteSchedule(scheduleUUID);
    this.setState({ scheduleUUID: null });
  };

  componentDidMount() {
    this.props.getSchedules();
    this.props.fetchUniverseList();
  }

  render() {
    const {
      customer: { schedules, deleteSchedule, currentCustomer },
      universe: { universeList, currentUniverse },
      closeModal,
      modal,
      modal: { visibleModal, showModal }
    } = this.props;

    const findUniverseName = (uuid) => {
      if (getPromiseState(universeList).isSuccess()) {
        const currentUniverse =
          universeList.data.find((universe) => universe.universeUUID === uuid) || null;
        if (currentUniverse) {
          return currentUniverse.name;
        }
      }
      return null;
    };

    let schedulesList = <span style={{ padding: '10px 15px' }}>There is no data to display</span>;
    if (
      getPromiseState(schedules).isSuccess() &&
      isNonEmptyArray(schedules.data) &&
      getPromiseState(universeList).isSuccess()
    ) {
      const filteredSchedules = schedules.data.filter((item) => {
        return item.taskParams.universeUUID === currentUniverse.data.universeUUID;
      });
      if (filteredSchedules.length) {
        schedulesList = filteredSchedules.map((scheduleItem, idx) => {
          const universeName = findUniverseName(scheduleItem.taskParams.universeUUID);
          if (universeName) {
            return (
              <ScheduleDisplayItem
                key={scheduleItem.name + scheduleItem.taskType + idx}
                idx={idx}
                modal={modal}
                handleDeleteAction={this.handleDeleteAction}
                name={universeName}
                schedule={scheduleItem}
                readOnly={isNonAvailable(currentCustomer.data.features, 'universes.backup')}
              />
            );
          }
          return null;
        });
      }
    }

    return (
      <div id="page-wrapper">
        <YBPanelItem
          className={'schedules-panel'}
          noBackground={true}
          header={
            <div className="container-title clearfix">
              <div className="pull-left">
                <h2 className="task-list-header content-title pull-left">Scheduled Backups</h2>
              </div>
              <div className="pull-right">
                {isAvailable(currentCustomer.data.features, 'universes.backup') && (
                  <div className="backup-action-btn-group">
                    <TableAction
                      className="table-action"
                      btnClass={'btn-orange'}
                      actionType="create-scheduled-backup"
                      isMenuItem={false}
                    />
                  </div>
                )}
              </div>
            </div>
          }
          body={
            <Fragment>
              <YBModalForm
                title={'Confirm Delete Schedule'}
                visible={showModal && visibleModal === 'deleteScheduleModal'}
                className={getPromiseState(deleteSchedule).isError() ? 'modal-shake ' : ''}
                onHide={closeModal}
                showCancelButton={true}
                cancelLabel={'Cancel'}
                onFormSubmit={this.handleDeleteSchedule}
              >
                Are you sure you want to delete reccuring event with ScheduleUUID{' '}
                {this.state.scheduleUUID}
                {getPromiseState(deleteSchedule).isError() &&
                  isNonEmptyObject(deleteSchedule.error) && (
                  <Alert bsStyle={'danger'} variant={'danger'}>
                    Schedule deleting went wrong:
                    <br />
                    {JSON.stringify(deleteSchedule.error)}
                  </Alert>
                )}
              </YBModalForm>
              <Row>{schedulesList}</Row>
            </Fragment>
          }
        />
      </div>
    );
  }
}

export default Schedules;
