import { Field, Formik } from 'formik';
import React from 'react';
import { Col, Row } from 'react-bootstrap';
import { reduxForm } from 'redux-form';
import { YBButton, YBMultiSelectWithLabel, YBTextInputWithLabel } from '../../common/forms/fields';
import { AddDestinationChannelFrom } from './AddDestinationChannelFrom';

const required = (value) => (value ? undefined : 'This field is required.');

const styles = {
  'add-destination-container': {
    position: 'relative',
    top: '40px'
  },
  'pd-0': {
    padding: '0px'
  },
  'alert-dest-add-link': {
    cursor: 'pointer',
    maginLeft: '-2px',
    lineHeight: '37px',
    fontSize: '15px',
    fontWeight: '500'
  }
};

const AlertDestinationConfiguration = (props) => {
  /**
   * Constant value of channel list.
   */
  const destinationChannelList = [
    { value: 'Configured slack channel', label: 'Configured slack channel' },
    { value: 'Configured email channel', label: 'Configured email channel' },
    { value: 'Configured pagerDuty channel', label: 'Configured pagerDuty channel' }
  ];
  /**
   *
   * @param {Formvalues} values
   * TODO: Make an API call to submit the form by reformatting the payload.
   */
  const handleOnSubmit = (values) => {
    // console.log(values)
  };

  const {
    modal: { visibleModal },
    onAddCancel
  } = props;
  return (
    <>
      <Formik initialValues={null}>
        <form name="alertDestinationForm" onSubmit={props.handleSubmit(handleOnSubmit)}>
          <Row className="config-section-header">
            <Row>
              <Col md={6}>
                <div className="form-item-custom-label">Destination Name</div>
                <Field
                  name="ALERT_DESTINATION_NAME"
                  placeHolder="Enter an alert destination"
                  component={YBTextInputWithLabel}
                  validate={required}
                  isReadOnly={false}
                />
              </Col>
            </Row>
            <Row>
              <Col md={6}>
                <div className="form-item-custom-label">Choose Channels</div>
                <Field
                  name="DESTINATION_CHANNEL_LIST"
                  component={YBMultiSelectWithLabel}
                  options={destinationChannelList}
                  hideSelectedOptions={false}
                  isMulti={true}
                />
              </Col>
              <Col md={6} style={styles['add-destination-container']}>
                <Row>
                  <Col lg={1} style={styles['pd-0']}>
                    <i
                      className="fa fa-plus-circle fa-2x on-prem-row-add-btn"
                      onClick={props.showAddChannelModal}
                    />
                  </Col>
                  <Col lg={3} style={styles['pd-0']}>
                    <a style={styles['alert-dest-add-link']} onClick={props.showAddChannelModal}>
                      Add Channel{' '}
                    </a>
                  </Col>
                </Row>
              </Col>
            </Row>
            <br/>
            <br/>
            <br/>
            <br/>
            <br/>
            <br/>
            <br/>
            <Row className="form-action-button-container">
              <Col lg={6} lgOffset={6}>
                <YBButton
                  btnText="Cancel"
                  btnClass="btn"
                  onClick={() => {
                    onAddCancel(false);
                  }}
                />
                <YBButton btnText="Save" btnType="submit" btnClass="btn btn-orange" />
              </Col>
            </Row>
          </Row>
        </form>
      </Formik>
      <AddDestinationChannelFrom
        visible={visibleModal === 'alertDestinationForm'}
        onHide={props.closeModal}
        defaultChannel="email"
      />
    </>
  );
};

export default reduxForm({
  form: 'alertDestinationForm',
  enableReinitialize: true
})(AlertDestinationConfiguration);
