
## Table of Contents

- [Structure of a documentation page](#structure-of-each-page)
- [Mandatory front matter attributes](#mandatory-front-matter-attributes)
- [Optional front matter attributes](#optional-front-matter-attributes)
- [Types of pages](#types-of-pages)
- [Widgets](#widgets)
  - [Information boxes](#information-boxes)
    - [Note box](#note-box)
    - [Tip box](#tip-box)
    - [Warning box](#warning-box)
  - [Inline section switcher](#inline-section-switcher)

# Structure of a documentation page

All documentation pages must start with front matter as shown below.

```
---
title: SEO-Title-Browser-Tab-Title
headerTitle: Doc-Page-Title
linkTitle: Navigation-Link
description: SEO-Meta-Description
image: Icon-For-Page-Title
headcontent: Brief-description
menu:
  latest:
    identifier: page-identifier
    parent: parent-page-identifier
    weight: number-to-decide-display-order
---
```

## Mandatory front matter attributes

| Field Name      | Description                                                    |
| :-------------- | :------------------------------------------------------------- |
| `title`         | Title text to display in browser tab and search engine results |
| `headerTitle`   | Title text to appear as the page title                         |
| `linkTitle`     | Title text to display in the navigation bar                    |
| `description`   | Description text to display in search engine results           |

## Optional front matter attributes

| Field Name      | Default | Description           |
| :-------------: | :-----: | --------------------- |
| `image`         | -       | Optional icon that is displayed next to the title |
| `isTocNested`   | `false` | Should sub-sections be displayed in the TOC on the right |
| `showAsideToc`  | `false` | Should the TOC on the right be enabled |
| `hidePagination`| `false` | Should the automatic navigation links be displayed at the bottom of the page |

# Types of pages

There are different types of documentation pages as noted below.

## Index pages

Index pages have links to subtopics within a topic. These pages are named `_index.html`.

## Content pages

Content pages contain information about topics. The names of these docs page has the format `my-docs-page.md`.

# Widgets

There are a number of display widgets available. These are listed below.

## Information boxes

### Note box

A note box gives some important information that is often not optional. It looks as follows:
![Note Box](https://raw.githubusercontent.com/yugabyte/docs/master/contributing/info-box-NOTE.png)

Hugo shortcode to add a note box:

```
{{< note title="Note" >}}

This is a note with a [link](https://www.yugabyte.com).

{{< /note >}}
```

### Tip box

A tip box gives a hint or other useful but optional piece of information. It looks as follows:
![Tip Box](https://raw.githubusercontent.com/yugabyte/docs/master/contributing/info-box-TIP.png)

Hugo shortcode to add a tip box:

```
{{< tip title="Tip" >}}

This is a tip with a [link](https://www.yugabyte.com).

{{< /tip >}}
```

### Warning box

A warning box informs the user about a potential issue or something to watch out for. It looks as follows:
![Warning Box](https://raw.githubusercontent.com/yugabyte/docs/master/contributing/info-box-WARNING.png)

Hugo shortcode to add a warning box:

```
{{< warning title="Warning" >}}

This is a warning with a [link](https://www.yugabyte.com).

{{< /warning >}}
```

## Inline Section Switcher

An inline section switcher lets you switch between content sections **without a separate URL***. If you want to link to sub-sections inside a switcher, use tabs. This widget looks as follows:

![Inline section switcher](https://raw.githubusercontent.com/yugabyte/docs/master/contributing/inline-section-switcher.png)

The corresponding code for this widget is shown below. Note that the actual content must be placed in a file with the `.md` extension inside a subdirectory whose name is easy to associate with the switcher title.

```
<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="#macos" class="nav-link active" id="macos-tab" data-toggle="tab"
       role="tab" aria-controls="macos" aria-selected="true">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>
  <li>
    <a href="#linux" class="nav-link" id="linux-tab" data-toggle="tab" 
       role="tab" aria-controls="linux" aria-selected="false">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>
  <li>
    <a href="#docker" class="nav-link" id="docker-tab" data-toggle="tab"
       role="tab" aria-controls="docker" aria-selected="false">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>
  <li >
    <a href="#kubernetes" class="nav-link" id="kubernetes-tab" data-toggle="tab"
       role="tab" aria-controls="kubernetes" aria-selected="false">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="macos" class="tab-pane fade show active" role="tabpanel" aria-labelledby="macos-tab">
    {{% includeMarkdown "binary/explore-ysql.md" /%}}
  </div>
  <div id="linux" class="tab-pane fade" role="tabpanel" aria-labelledby="linux-tab">
    {{% includeMarkdown "binary/explore-ysql.md" /%}}
  </div>
  <div id="docker" class="tab-pane fade" role="tabpanel" aria-labelledby="docker-tab">
    {{% includeMarkdown "docker/explore-ysql.md" /%}}
  </div>
  <div id="kubernetes" class="tab-pane fade" role="tabpanel" aria-labelledby="kubernetes-tab">
    {{% includeMarkdown "kubernetes/explore-ysql.md" /%}}
  </div>
</div>
```
