import _ from 'lodash';
import React, { FC, useState } from 'react';
import { Input } from '../Input/Input';
import { PlusButton } from '../PlusButton/PlusButton';
import { ReactComponent as DeleteIcon } from './clear-24px.svg';
import { translate } from '../I18n/I18n';
import './KeyValueInput.scss';

type ValueType = Record<string, string | number>;

interface RowItem {
  key: string;
  value: string | number;
}

interface KeyValueInputProps {
  value: ValueType;
  onChange(value: ValueType): void;
  placeholderKey?: string;
  placeholderValue?: string;
  disabled?: boolean;
}

const objectToArray = (data: ValueType): RowItem[] => {
  const result: RowItem[] = [];
  for (const [key, value] of Object.entries(data)) {
    result.push({ key, value });
  }

  // add empty row placeholder if there are no records so far
  if (!result.length) result.push({ key: '', value: '' });

  return result;
};

const arrayToObject = (data: RowItem[]): ValueType => {
  const result: ValueType = {};
  data.forEach(({ key, value }) => {
    // skip empty rows
    if (key) result[key] = value;
  });
  return result;
};

export const KeyValueInput: FC<KeyValueInputProps> = ({
  placeholderKey,
  placeholderValue,
  value,
  onChange,
  disabled
}) => {
  const [internalValue, setInternalValue] = useState<RowItem[]>(objectToArray(value));

  const addRow = () => updateData([...internalValue, { key: '', value: '' }]);
  const deleteRow = (item: RowItem) => updateData(_.without(internalValue, item));
  const updateData = (newData: RowItem[]) => {
    if (!disabled) {
      onChange(arrayToObject(newData));
      setInternalValue(newData);
    }
  };

  return (
    <div className="key-value-input">
      {internalValue.map((row, index) => (
        <div key={index} className="key-value-input__row">
          <Input
            type="text"
            placeholder={placeholderKey}
            disabled={disabled}
            className="key-value-input__input"
            value={row.key}
            onChange={(event) => {
              const newValue = [...internalValue];
              newValue[index].key = event.target.value;
              updateData(newValue);
            }}
          />
          <Input
            type="text"
            placeholder={placeholderValue}
            disabled={disabled}
            className="key-value-input__input"
            value={row.value}
            onChange={(event) => {
              const newValue = [...internalValue];
              newValue[index].value = event.target.value;
              updateData(newValue);
            }}
          />
          {!disabled && (
            <DeleteIcon className="key-value-input__icon" onClick={() => deleteRow(row)} />
          )}
        </div>
      ))}

      {!disabled && (
        <PlusButton text="Add Row" className="key-value-input__add-row-btn" onClick={addRow} />
      )}
    </div>
  );
};
