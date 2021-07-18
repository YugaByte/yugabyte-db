import React, { useEffect, useState } from 'react';
import { Formik } from 'formik';
import { Col, Row } from 'react-bootstrap';
import { Field, reduxForm } from 'redux-form';
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
  const [destinationChannelList, setDestinationChannelList] = useState([]);

  useEffect(() => {
    props.getAlertReceivers().then((receivers) => {
      receivers = receivers.map((receiver) => {
        return {
          value: receiver['uuid'],
          label: receiver['name']
        };
      });
      setDestinationChannelList(receivers);
    });
  }, []);

  /**
   *
   * @param {Formvalues} values
   * TODO: Make an API call to submit the form by reformatting the payload.
   */
  const handleOnSubmit = (values) => {
    let payload = {
      name: '',
      receivers: [],
      defaultRoute: true
    };
    payload.name = values['ALERT_DESTINATION_NAME'];
    values['DESTINATION_CHANNEL_LIST'].forEach((channel) => payload.receivers.push(channel.value));
    props.setInitialValues();
    values.type === 'update'
      ? props.updateAlertDestination(payload, values.uuid).then(() => props.onAddCancel())
      : props.createAlertDestination(payload).then(() => props.onAddCancel());
  };

  const {
    handleSubmit,
    onAddCancel,
    setInitialValues,
    initialValues,
    modal: { showModal, visibleModal }
  } = props;

  return (
    <>
      <Formik initialValues={initialValues}>
        <form name="alertDestinationForm" onSubmit={handleSubmit(handleOnSubmit)}>
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
                  validate={required}
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
            <br />
            <br />
            <br />
            <br />
            <br />
            <br />
            <br />
            <Row className="alert-action-button-container">
              <Col lg={6} lgOffset={6}>
                <YBButton
                  btnText="Cancel"
                  btnClass="btn"
                  onClick={() => {
                    setInitialValues();
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
        visible={showModal && visibleModal === 'alertDestinationForm'}
        onHide={props.closeModal}
        defaultChannel="email"
        updateDestinationChannel={setDestinationChannelList}
        {...props}
      />
    </>
  );
};

export default reduxForm({
  form: 'alertDestinationForm',
  enableReinitialize: true
})(AlertDestinationConfiguration);
