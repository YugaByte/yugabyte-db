import React, { useRef } from 'react';

export const GFlagEditor = ({ fields, name }) => {
  const gflagsEditor = useRef();

  const handleBlur = () => {
    // Need to store the objects in new array because we are clearing the
    // original `fields` array before adding the new parsed key-value pairs
    const parsedFields = [];
    if (gflagsEditor.current) {      
      const editorText = gflagsEditor.current.innerText;
      const textArray = editorText.trim().split('\n');
      // Regex for testing whether line contains a key string,
      // a colon between, and then value string. This value string 
      // must not contain any whitespace or must be double/single quoted.
      const gflagRegex = /^([a-zA-Z0-9_]+)\s*?[:=]\s*?("(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*'|\S+)\s*$/;
      textArray.forEach(text => {
        const match = gflagRegex.exec(text);
        if (match) {
          // eslint-disable-next-line no-unused-vars
          const [_, name, value] = match;          
          parsedFields.push({
            name,
            value
          });
        }
      });
      fields.removeAll();
      parsedFields.forEach(obj => fields.push(obj));
      gflagsEditor.current.innerHTML = fields.map((field, idx) => {
        const flagObj = fields.get(idx);
        if ('name' in flagObj) {
          return `<p key="${field + idx}">
            <strong>${flagObj.name}:</strong> ${flagObj.value}
          </p>`;
        }            
        return '';
      }).join('');
    }
  };

  // Generate raw HTML text
  const html = fields.map((field, idx) => {
    const flagObj = fields.get(idx);
    if ('name' in flagObj) {
      return `<p key="${field + idx}">
        <strong>${flagObj.name}:</strong> ${flagObj.value}
      </p>`;
    }            
    return '';
  }).join('');

  return (
    <div contentEditable
      name={name}
      className="gflag-array__editor"
      ref={gflagsEditor}
      onBlur={handleBlur}
      dangerouslySetInnerHTML={{
        __html: html
      }}
    ></div>
  );
};
