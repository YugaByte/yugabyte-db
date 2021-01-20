import React, { Component } from 'react';
import hljs from 'highlight.js/lib/highlight';
import json from 'highlight.js/lib/languages/json';
import sql from 'highlight.js/lib/languages/sql';

export class Highlighter extends Component {
  constructor(props) {
    super();
    if (props.type === 'json') {
      hljs.registerLanguage('json', json);
    } else if (props.type === 'sql') {
      hljs.registerLanguage('sql', sql);
    }
  }
  render() {
    const { text, type, element } = this.props;
    if (element === 'pre') {
      return (
        <pre
          {...this.props}
          dangerouslySetInnerHTML={{
            __html: hljs.highlight(type, text).value
          }}
        />
      );
    }
    return (
      <div
        {...this.props}
        dangerouslySetInnerHTML={{
          __html: hljs.highlight(type, text).value
        }}
      />
    );
  }
}
