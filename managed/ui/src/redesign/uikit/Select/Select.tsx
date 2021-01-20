import React from 'react';
import ReactSelect, { Styles, Props } from 'react-select';

const customStyles: Styles = {
  control: (provided, state) => ({
    ...provided,
    outline: 'none',
    // hack to show red border on validation error as there's no other way to forward extra prop into styles
    border: `1px solid ${
      state.selectProps.className === 'validation-error' ? '#FF0000' : '#C5C6CE'
    }`,
    borderRadius: '7px',
    overflow: 'hidden',
    minHeight: '44px',
    width: '100%',
    backgroundColor: state.isDisabled ? '#C5C6CE' : '#fff',
    boxShadow:
      state.selectProps.className === 'validation-error' ? '0 0 5px rgba(255, 0, 0, 0.2)' : 'none',
    ':hover': {
      border: `1px solid ${
        state.selectProps.className === 'validation-error' ? '#FF0000' : '#C5C6CE'
      }`,
      boxShadow:
        state.selectProps.className === 'validation-error' ? '0 0 5px rgba(255, 0, 0, 0.2)' : 'none'
    },
    fontSize: '16px',
    fontWeight: 400,
    fontFamily: '"Source Sans Pro", sans-serif'
  }),
  valueContainer: (provided, state) => ({
    ...provided,
    padding: 0,
    paddingLeft: state.hasValue ? (state.isMulti ? '5px' : '20px') : '20px'
  }),
  placeholder: (provided) => ({
    ...provided,
    color: '#546371',
    opacity: 0.5
  }),
  singleValue: (provided) => ({
    ...provided,
    color: '#546371'
  }),
  multiValue: (provided) => ({
    ...provided,
    backgroundColor: '#E8E9F3',
    borderRadius: '7px',
    overflow: 'hidden'
  }),
  multiValueLabel: (provided) => ({
    ...provided,
    // have to provide every padding separately in order to override provided defaults
    paddingTop: '3px',
    paddingBottom: '3px',
    paddingLeft: '12px',
    paddingRight: '3px',
    fontSize: '16px',
    color: '#546371'
  }),
  multiValueRemove: (provided) => ({
    ...provided,
    cursor: 'pointer',
    color: '#5463717F',
    ':hover': {
      color: '#546371'
    }
  }),
  clearIndicator: (provided) => ({
    ...provided,
    cursor: 'pointer',
    color: '#5463717F',
    ':hover': {
      color: '#546371'
    }
  }),
  indicatorSeparator: (provided) => ({
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
  menu: (provided) => ({
    ...provided,
    fontSize: '16px',
    fontWeight: 400,
    fontFamily: '"Source Sans Pro", sans-serif'
  }),
  groupHeading: (provided) => ({
    ...provided,
    color: '#546371',
    fontSize: '16px',
    fontWeight: 700,
    fontFamily: '"Source Sans Pro", sans-serif'
  })
};

export const Select = <T extends {}>(props: Props<T>) => (
  <ReactSelect<T> styles={customStyles} {...props} />
);
