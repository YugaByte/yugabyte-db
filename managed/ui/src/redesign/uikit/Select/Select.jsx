import React from 'react';
import ReactSelect from 'react-select';

const customStyles = {
  control: (provided, state) => ({
    ...provided,
    outline: 'none',
    border: '1px solid #C5C6CE',
    borderRadius: '29px',
    overflow: 'hidden',
    minHeight: '44px',
    width: '100%',
    backgroundColor: state.isDisabled ? '#C5C6CE' : '#fff',
    boxShadow: 'none',
    ':hover': {
      borderColor: '#C5C6CE',
    },
    fontSize: '16px',
    fontWeight: 400,
    fontFamily: '"Source Sans Pro", sans-serif'
  }),
  valueContainer: (provided, state) => ({
    ...provided,
    padding: 0,
    paddingLeft: state.hasValue
      ? (state.isMulti ? '5px' : '20px')
      : '20px'
  }),
  placeholder: provided => ({
    ...provided,
    color: '#546371',
    opacity: 0.5
  }),
  singleValue: provided => ({
    ...provided,
    color: '#546371'
  }),
  multiValue: provided => ({
    ...provided,
    backgroundColor: '#E8E9F3',
    borderRadius: '29px',
    overflow: 'hidden',
  }),
  multiValueLabel: provided => ({
    ...provided,
    // have to provide every padding separately in order to override provided defaults
    paddingTop: '3px',
    paddingBottom: '3px',
    paddingLeft: '12px',
    paddingRight: '6px',
    fontSize: '16px',
    color: '#546371'
  }),
  multiValueRemove: provided => ({
    ...provided,
    cursor: 'pointer',
    color: '#5463717F',
    ':hover': {
      color: '#546371',
    }
  }),
  clearIndicator: provided => ({
    ...provided,
    cursor: 'pointer',
    color: '#5463717F',
    ':hover': {
      color: '#546371',
    }
  }),
  indicatorSeparator: provided => ({
    ...provided,
    backgroundColor: '#C5C6CE'
  }),
  dropdownIndicator: (provided, state) => ({
    ...provided,
    cursor: 'pointer',
    marginTop: '2px',
    marginLeft: '12px',
    marginRight: '14px',
    padding: 0,
    width: 0,
    height: 0,
    borderLeft: '6px solid transparent',
    borderRight: '6px solid transparent',
    borderTop: state.isDisabled ? '7px solid #fff' : '7px solid #5463717F',
    ':hover': {
      borderTopColor: '#546371'
    }
  }),
  menu: provided => ({
    ...provided,
    fontSize: '16px',
    fontWeight: 400,
    fontFamily: '"Source Sans Pro", sans-serif'
  }),
};

export const Select = props => {
  const extendedProps = {
    ...props,
    className: `wizard-select ${props.className || ''}`
  };

  return (
    <ReactSelect
      styles={customStyles}
      {...extendedProps}
    />
  );
};
