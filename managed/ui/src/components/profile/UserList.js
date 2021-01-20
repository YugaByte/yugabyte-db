// Copyright (c) YugaByte, Inc.

import React, { Component } from 'react';
import { Row, Col, DropdownButton, MenuItem } from 'react-bootstrap';
import { BootstrapTable, TableHeaderColumn } from 'react-bootstrap-table';
import { YBPanelItem } from '../panels';
import { connect } from 'react-redux';
import { getPromiseState } from '../../utils/PromiseUtils';
import { YBLoading } from '../common/indicators';
import { YBButton } from '../../components/common/forms/fields';
import { openDialog, closeDialog } from '../../actions/modal';
import { AddUserModal } from './modals/AddUserModal';
import { EditRoleModal } from './modals/EditRoleModal';
import { YBConfirmModal } from '../modals';
import { isNotHidden, isDisabled } from '../../utils/LayoutUtils';
import {
  changeUserRole,
  createUser,
  createUserResponse,
  deleteUser,
  deleteUserResponse
} from '../../actions/customers';

class UserList extends Component {
  constructor(props) {
    super(props);
    this.state = { userForModal: null };
  }

  showDeleteUserModal(user) {
    this.setState({ userForModal: user });
    this.props.showConfirmDeleteModal();
  }

  editRole(user) {
    this.setState({ userForModal: user });
    this.props.showEditRoleModal();
  }

  doDeleteUser = async () => {
    const user = this.state.userForModal;
    try {
      await this.props.deleteUser(user.uuid);
    } catch (error) {
      console.error('Failed to delete user', error);
    } finally {
      this.props.getCustomerUsers();
      this.props.closeModal();
    }
  };

  actionsDropdown = (user) => {
    const { customer } = this.props;
    const loginUserId = localStorage.getItem('userId');
    if (
      user.uuid === loginUserId ||
      isDisabled(customer.data.features, 'universe.create') ||
      user.role === 'SuperAdmin'
    ) {
      return null;
    } else {
      return (
        <DropdownButton
          className="btn btn-default"
          title="Actions"
          id="bg-nested-dropdown"
          pullRight
        >
          <MenuItem onClick={() => this.editRole(user)}>
            <span className="fa fa-edit" /> Edit User Role
          </MenuItem>
          <MenuItem onClick={() => this.showDeleteUserModal(user)}>
            <span className="fa fa-trash" /> Delete User
          </MenuItem>
        </DropdownButton>
      );
    }
  };

  render() {
    const {
      users,
      customer,
      modal: { visibleModal, showModal },
      showAddUserModal,
      closeModal
    } = this.props;

    if (getPromiseState(users).isLoading() || getPromiseState(users).isInit()) {
      return <YBLoading />;
    }

    return (
      <div className="user-list-container">
        <Row>
          <Col className="pull-left">
            <h2>Users</h2>
          </Col>
          <Col className="pull-right">
            {isNotHidden(customer.data.features, 'universe.create') && (
              <YBButton
                btnClass="universe-button btn btn-lg btn-orange"
                onClick={showAddUserModal}
                btnText="Add User"
                btnIcon="fa fa-plus"
                disabled={isDisabled(customer.data.features, 'universe.create')}
              />
            )}
            {/* re-mount modals in order to internal forms show valid initial values all the times */}
            {showModal && visibleModal === 'addUserModal' && (
              <AddUserModal
                modalVisible
                onHide={closeModal}
                createUser={this.props.createUser}
                getCustomerUsers={this.props.getCustomerUsers}
              />
            )}
            {showModal && visibleModal === 'editRoleModal' && (
              <EditRoleModal
                modalVisible
                onHide={closeModal}
                user={this.state.userForModal}
                changeUserRole={this.props.changeUserRole}
                getCustomerUsers={this.props.getCustomerUsers}
              />
            )}
            <YBConfirmModal
              name="deleteUserModal"
              title="Delete User"
              hideConfirmModal={closeModal}
              currentModal="deleteUserModal"
              visibleModal={visibleModal}
              onConfirm={this.doDeleteUser}
              confirmLabel="Delete"
              cancelLabel="Cancel"
            >
              Are you sure you want to delete user <strong>{this.state.userForModal?.email}</strong>{' '}
              ?
            </YBConfirmModal>
          </Col>
        </Row>
        <Row>
          <YBPanelItem
            body={
              <BootstrapTable
                data={users.data}
                bodyStyle={{ marginBottom: '1%', paddingBottom: '1%' }}
              >
                <TableHeaderColumn dataField="uuid" isKey hidden />
                <TableHeaderColumn dataField="email">Email</TableHeaderColumn>
                <TableHeaderColumn dataField="role">Role</TableHeaderColumn>
                <TableHeaderColumn dataField="creationDate">Created At</TableHeaderColumn>
                <TableHeaderColumn
                  columnClassName="table-actions-cell"
                  dataFormat={(cell, row) => this.actionsDropdown(row)}
                >
                  Actions
                </TableHeaderColumn>
              </BootstrapTable>
            }
          />
        </Row>
      </div>
    );
  }
}

const mapDispatchToProps = (dispatch) => {
  return {
    createUser: (payload) => {
      return dispatch(createUser(payload)).then((response) => {
        return dispatch(createUserResponse(response.payload));
      });
    },
    changeUserRole: (userUUID, newRole) => {
      return dispatch(changeUserRole(userUUID, newRole));
    },
    deleteUser: (userUUID) => {
      return dispatch(deleteUser(userUUID)).then((response) => {
        return dispatch(deleteUserResponse(response.payload));
      });
    },
    showAddUserModal: () => {
      dispatch(openDialog('addUserModal'));
    },
    showEditRoleModal: () => {
      dispatch(openDialog('editRoleModal'));
    },
    showConfirmDeleteModal: () => {
      dispatch(openDialog('deleteUserModal'));
    },
    closeModal: () => {
      dispatch(closeDialog());
    }
  };
};

function mapStateToProps(state) {
  return {
    users: state.customer.users,
    modal: state.modal
  };
}

export default connect(mapStateToProps, mapDispatchToProps)(UserList);
