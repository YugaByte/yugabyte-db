import ReactDOM from 'react-dom';
import React, { FC, useLayoutEffect, useRef, useState } from 'react';
import { useHoverDirty } from 'react-use';
import './Tooltip.scss';

const docBody = document.body;

export const Tooltip: FC = ({ children }) => {
  const handle = useRef<HTMLDivElement>(null);
  const isHovering = useHoverDirty(handle);
  const [top, setTop] = useState<number>(0);
  const [left, setLeft] = useState<number>(0);

  // update tooltip position to show it right below the tooltip handle
  useLayoutEffect(() => {
    if (isHovering && handle.current) {
      const handleRect = handle.current.getBoundingClientRect();
      setTop(handleRect.bottom);
      setLeft(handleRect.left);
    }
  }, [isHovering]);

  // render tooltip popup via portal to properly show it when any parent element has "overflow: hidden"
  return (
    <div className="yb-uikit-tooltip">
      <div className="yb-uikit-tooltip__handle" ref={handle} />
      {ReactDOM.createPortal(
        <div className="yb-uikit-tooltip-popup" style={{ left, top, opacity: isHovering ? 1 : 0 }}>
          {children}
        </div>,
        docBody
      )}
    </div>
  );
};
