import React, { FC } from 'react';
import { CommunicationPorts } from '../../../../helpers/dtos';
import { I18n } from '../../../../uikit/I18n/I18n';
import { Input } from '../../../../uikit/Input/Input';
import './CommunicationPortsEditor.scss';

export const defaultPorts: CommunicationPorts = {
  masterHttpPort: 7000,
  masterRpcPort: 7100,
  tserverHttpPort: 9000,
  tserverRpcPort: 9100,
  redisServerHttpPort: 11000,
  redisServerRpcPort: 6379,
  yqlServerHttpPort: 12000,
  yqlServerRpcPort: 9042,
  ysqlServerHttpPort: 13000,
  ysqlServerRpcPort: 5433,
  nodeExporterPort: 9300
};

const portsConfig = [
  { label: 'Master HTTP Port', prop: 'masterHttpPort' },
  { label: 'Master RPC Port', prop: 'masterRpcPort' },
  { label: 'T-Server HTTP Port', prop: 'tserverHttpPort' },
  { label: 'T-Server RPC Port', prop: 'tserverRpcPort' },
  { label: 'Yedis HTTP Port', prop: 'redisServerHttpPort' },
  { label: 'Yedis RPC Port', prop: 'redisServerRpcPort' },
  { label: 'YCQL HTTP Port', prop: 'yqlServerHttpPort' },
  { label: 'YCQL RPC Port', prop: 'yqlServerRpcPort' },
  { label: 'YSQL HTTP Port', prop: 'ysqlServerHttpPort' },
  { label: 'YSQL RPC Port', prop: 'ysqlServerRpcPort' },
  { label: 'Node Exporter Port', prop: 'nodeExporterPort' }
];

interface CommunicationPortsEditorProps {
  value: CommunicationPorts;
  onChange(value: CommunicationPorts): void;
  disabled?: boolean;
}

const MAX_PORT = 65535;

export const CommunicationPortsEditor: FC<CommunicationPortsEditorProps> = ({
  value,
  onChange,
  disabled
}) => {
  return (
    <div className="communication-ports-editor">
      {portsConfig.map((item) => (
        <div key={item.prop} className="communication-ports-editor__item">
          <div className="communication-ports-editor__label">
            <I18n>{item.label}</I18n>
          </div>
          <div className="communication-ports-editor__input">
            <Input
              disabled={disabled}
              className={
                Number(value[item.prop]) === Number(defaultPorts[item.prop])
                  ? ''
                  : 'communication-ports-editor__overridden-value'
              }
              value={value[item.prop]}
              onChange={(event) => onChange({ ...value, [item.prop]: event.target.value })}
              onBlur={(event) => {
                let port = Number(event.target.value.replace(/\D/g, '')) || defaultPorts[item.prop];
                port = port > MAX_PORT ? MAX_PORT : port;
                onChange({ ...value, [item.prop]: port });
              }}
            />
          </div>
        </div>
      ))}
    </div>
  );
};
