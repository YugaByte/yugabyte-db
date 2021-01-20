// Copyright (c) YugaByte, Inc.

import React from 'react';
import LogoFull from './images/yb_yblogo_darkbg.svg';
import LogoMonochrome from './images/yb_yblogo_whitebg.svg';
import LogoIcon from './images/yb_ybsymbol_original.svg';

function YBLogo(props) {
  if (props.type === 'full') {
    return (
      <object className="logo" data={LogoFull} type="image/svg+xml">
        Yugabyte Logo
      </object>
    );
  }
  if (props.type === 'monochrome') {
    return (
      <object className="logo" data={LogoMonochrome} type="image/svg+xml">
        Yugabyte Logo
      </object>
    );
  }
  return (
    <object className="logo" data={LogoIcon} type="image/svg+xml">
      Yugabyte Logo
    </object>
  );
}

export default YBLogo;
