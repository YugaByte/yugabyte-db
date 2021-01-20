import React, { useEffect, useState } from 'react';
import clsx from 'clsx';
import './MenuItemsContainer.scss';

/**
 * @param {Object} props
 * @param {boolean} props.parentDropdownOpen
 * @param {() => React.ReactNode} props.mainMenu
 * @param {Record<string, () => React.ReactNode>} props.subMenus
 */
export const MenuItemsContainer = ({ mainMenu, subMenus, parentDropdownOpen }) => {
  const [activeSubmenu, setActiveSubmenu] = useState(null);

  // reset active submenu when parent dropdown closed
  useEffect(() => {
    if (!parentDropdownOpen && activeSubmenu !== null) setActiveSubmenu(null);
  }, [parentDropdownOpen, activeSubmenu]);

  const backToMainMenu = () => setActiveSubmenu(null);

  return (
    <div className="menu-items-container">
      <div className={clsx(
        'menu-items-container__main-frame', {
          'menu-items-container__main-frame--slide-right': activeSubmenu === null,
          'menu-items-container__main-frame--slide-left': activeSubmenu !== null
        }
      )}>
        {mainMenu(setActiveSubmenu)}
      </div>
      <div className="menu-items-container__submenu-frame">
        {activeSubmenu && subMenus[activeSubmenu](backToMainMenu)}
      </div>
    </div>
  );
};
