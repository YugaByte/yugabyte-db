import React, { useRef, useState } from 'react';
import { useQuery } from 'react-query';
import * as Yup from 'yup';
import { Field } from 'formik';
import { YBButton, YBFormInput, YBFormToggle } from '../../common/forms/fields';
import { YBModalForm } from '../../common/forms';
import { YBLoadingCircleIcon } from '../../common/indicators';
import {
  createAlertDefinition,
  getAlertDefinition,
  updateAlertDefinition
} from '../../../actions/universe';

const ALERT_NAME = 'Replication Lag Alert';
const ALERT_TEMPLATE = 'REPLICATION_LAG';
const DEFAULT_THRESHOLD = 180000;

const DEFAULT_FORM_VALUE = {
  enableAlert: false,
  lagThreshold: DEFAULT_THRESHOLD
};

const validationSchema = Yup.object().shape({
  enableAlert: Yup.boolean(),
  lagThreshold: Yup.number().when('enableAlert', {
    is: true,
    then: Yup.number().typeError('Must be a number').required('Required Field'),
    otherwise: Yup.mixed()
  })
});

const parseThresholdFromQuery = (query) => {
  const threshold = query.split('>').pop().trim();
  return isNaN(threshold) ? DEFAULT_THRESHOLD : threshold;
};

export const ReplicationAlertModalBtn = ({ universeUUID, disabled }) => {
  const formik = useRef();
  const [isModalVisible, setModalVisible] = useState(false);
  const [alertDefinitionUUID, setAlertDefinitionUUID] = useState(null);
  const [submissionError, setSubmissionError] = useState();

  const { isFetching } = useQuery(
    ['getAlertDefinition', universeUUID, ALERT_NAME],
    () => getAlertDefinition(universeUUID, ALERT_NAME),
    {
      retry: false, // no need to retry a failed query as 400 response is OK when there's no alert definition yet
      refetchOnWindowFocus: false,
      enabled: isModalVisible,
      onSuccess: (data) => {
        setAlertDefinitionUUID(data.uuid);

        // update form value via workaround as initial form value inside <YBModalForm> is set when
        // it rendered for the first time and we don't have an API response at that time yet
        formik.current.setValues({
          enableAlert: data.isActive,
          lagThreshold: parseThresholdFromQuery(data.query)
        });
      }
    }
  );

  const toggleModalVisibility = () => {
    if (isModalVisible) {
      // don't close modal when form is submitting
      if (!formik.current.isSubmitting) {
        setModalVisible(false);
        setSubmissionError(null);
      }
    } else {
      setModalVisible(true);
    }
  };

  const submit = async (values, formikBag) => {
    const payload = {
      name: ALERT_NAME,
      template: ALERT_TEMPLATE,
      isActive: values.enableAlert,
      value: values.lagThreshold
    };

    try {
      if (alertDefinitionUUID) {
        await updateAlertDefinition(alertDefinitionUUID, payload);
      } else {
        await createAlertDefinition(universeUUID, payload);
      }

      formikBag.setSubmitting(false);
      toggleModalVisibility();
    } catch (error) {
      setSubmissionError(error.message);
      formikBag.setSubmitting(false);
    }
  };

  return (
    <>
      <YBButton
        disabled={disabled}
        btnClass="btn btn-orange"
        btnText="Configure Alert"
        onClick={toggleModalVisibility}
      />

      <YBModalForm
        initialValues={DEFAULT_FORM_VALUE}
        validationSchema={validationSchema}
        onFormSubmit={submit}
        onHide={toggleModalVisibility}
        title="Configure Replication Alert"
        visible={isModalVisible}
        error={submissionError}
        showCancelButton
        submitLabel="Save"
        render={(formikProps) => {
          // workaround to access form methods outside of <Formik> render method
          formik.current = formikProps;

          return isFetching ? (
            <YBLoadingCircleIcon size="medium" />
          ) : (
            <>
              <Field
                name="enableAlert"
                component={YBFormToggle}
                label="Enable Alert"
                subLabel="Notify if average lag is above threshold"
              />
              <br />
              <Field
                name="lagThreshold"
                component={YBFormInput}
                type="number"
                label="Threshold (ms)"
                disabled={!formikProps.values.enableAlert}
              />
            </>
          );
        }}
      />
    </>
  );
};
