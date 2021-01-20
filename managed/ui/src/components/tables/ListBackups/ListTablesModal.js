import React from 'react';
import { YBModal } from '../../common/forms/fields';

const ListTablesModal = ({ title, visible, data, onHide }) => {
  return (
    <div>
      <YBModal title={title} visible={visible} onHide={onHide} size="small">
        <div className="list-tables-modal__content ">
          <ul>{data && data.map((name) => <li>{name}</li>)}</ul>
        </div>
      </YBModal>
    </div>
  );
};

export default ListTablesModal;
