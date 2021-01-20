# Contributing to the YugabyteDB Documentation

This account has three sections:

&nbsp;&nbsp;&nbsp;[0. The art and craft of writing technical documentation](#0-the-art-and-craft-of-writing-technical-documentation)<br>
&nbsp;&nbsp;&nbsp;[1. Basic usage](#1-basic-usage)<br>
&nbsp;&nbsp;&nbsp;[2. Advanced usage](#2-advanced-usage)<br>
&nbsp;&nbsp;&nbsp;[3. Mental model and glossary: creating and maintaining syntax diagrams for YSQL Docs](#3-mental-model-and-glossary-creating-and-maintaining-syntax-diagrams-for-ysql-docs)

> **Note:** The account is intended mainly for Yugabyte staff. It assumes that you will make changes only in the `/latest/` subtree. You can easily generalise the account if you need to make changes in the `/stable/` tree. Both the `/latest/` and the `/stable/` subtrees are direct children of the [_content directory_](#content-directory). However, deciding the _policy_ for this is outside the scope of this document. You must establish the aims in this space in discussion with colleagues.

All the source material for the YugabyteDB documentation is held in the main [yugabyte-db](https://github.com/yugabyte/yugabyte-db) GitHub repository. Users read this documentation by starting at [docs.yugabyte.com/](https://docs.yugabyte.com/).

If you want to make changes to existing documentation, or to add new content, make sure first to [open a GitHub issue](https://github.com/yugabyte/yugabyte-db/issues/new). Then reference the Issue Number in your eventual Pull Request. Start the title with `[DOC]` and select the label `area/documentation` By clicking the gearwheel icon at "**Labels**" in the list of classifications on the right.

YugabyteDB docs are based on the "hugo" framework and use the "Material Docs" theme.

- [Hugo framework](https://gohugo.io/getting-started/)

- [Material Docs](https://theme-hugo-material-docs-demo.aerobaticapp.com/getting-started/)

## 0. The art and craft of writing technical documentation

Writing excellent technical documentation isn't easy. The challenge is very much greater when a single corpus is authored my many contributors—especially when some are experienced professional technical writers and others are engineers with little experience of writing good English prose. Moreover, opinions on style changes almost as fast as middle tier frameworks! Here are two useful references.

### The Sense of Style, by Steven Pinker (2014)

This is a highly recommended relatively recent work. You can [download the PDF](http://xidian-usa.org/wp-content/uploads/2019/07/The-Sense-of-Style.pdf) without charge.

### Google developer documentation style guide

The [Google developer documentation style guide](https://developers.google.com/style) is far more prescriptive than Pinker's book and is limited to the sub-genre of documentation to which the YugabyteDB corpus belongs. It's also a recommended read.

## 1. Basic usage

In this kind of work, the nature of your documentation change means that you will not need to modify existing syntax diagrams or to add new ones.

### Initial setup

Follow these steps if this is the first time you are setting up to work on the docs locally.

> **Note**: Many Yugabyte engineers use an Apple Mac computer; and many have taken the bold step of upgrading to MacOS Catalina—or even to Big Sur. The move from Mojave to Catalina brought all sorts of changes that cannot be resisted and that seem to make things suddenly "just not work". In particular, `git` commands and  `npm ci` (see below) fail. The error messages are inscrutable. For example `npm ci` produces pages of errors including ones like this:

   ```
   No receipt for 'com.apple.pkg.DeveloperToolsCLI' ...
   ```

> Some Internet search leads to a consensus that the fix is to (re)install an Apple component called "Xcode" from [this Apple site for downloading tools for Apple Developers](https://developer.apple.com/download/more/). Look for "Xcode" in the list, choose the latest production version, download the `.dmg` file (or `.xip` for Big Sur), double-click on it, and follow the on-screen instructions. It is recommended that you check periodically to make sure that you stay on the current version. (The usual paradigm that alerts you when a new version of installed software becomes available seems to be unreliable for "Xcode".)
>
> This business is yet more tedious after upgrading to MacOS Big Sur. Look at the Stack Exchange article [Git is not working after macOS Update...](https://stackoverflow.com/questions/52522565/git-is-not-working-after-macos-update-xcrun-error-invalid-active-developer-pa) and look for this:
>
> _"With any major or semi-major [MacOS] update you'll need to update the command line tools in order to get them functioning properly again. Check Xcode with any update."_
>
> The (re)installation of Xcode from the referenced  [tools for Apple Developers](https://developer.apple.com/download/more/) site is a bit different for Big Sur than it is for Catalina. The downloaded file is a `.xip` — an Apple proprietary compressed file, digitally signed for integrity. `Xcode_12.2.xip` is 11.43 GB and took ~ 30 minutes to download with a 175 MBPS download speed. Then it must be expanded—and that takes time too. This simply leaves you with `Xcode.app` in your "downloads" folder. It's about 30 MB (so who knows why the `.xip` is so ginormous). Anyway, drop it into the `/applications` folder. But even this is not enough. Then you need to do `xcode-select --install` at the command line prompt. It responds with "xcode-select: note: install requested for command line developer tools". You will then be prompted in an alert window to update Xcode Command Line tools—and this takes a long time too (maybe as much as 30 minutes). Only now do `git` commands start to work normally. Now, too (but not before) `npm ci` works properly.


Then:

Fork the yugabyte-db GitHub repository and create a local clone of your fork with a command like this:

```
git clone https://github.com/<YOUR_GITHUB_ID>/yugabyte-db.git
```

Identify your fork as `origin` and the original YB repository as `upstream`:

```
cd <your path>/yugabyte-db/
git remote set-url origin git@github.com:<YOUR_GITHUB_ID>/yugabyte-db.git
git remote add upstream git@github.com:YugaByte/yugabyte-db.git
```

For belt-and-braces, make sure that your local git is still current with the YB repo:

```
git checkout master
git pull upstream master
```

Install "hugo". For example, on a Mac, you can run the following commands:

```
brew update
brew install hugo
brew install npm
```

Install node modules thus:

```
cd docs
npm ci
```

Create a branch for your doc project and switch into it:

```
git branch feature_branch_name
git checkout feature_branch_name
```

It's recommended that you routinely check that you have the current latest "Xcode". You should also ensure that you have the current latest "brew", and the current latest "hugo", thus:

```
brew update
brew upgrade hugo
npm ci   
```
You _must_ do this if ever you drop your local `git` (for example after a successful `git push` and subsequent merge) and re-do the `git clone` step.

You might also find yourself needing to do this if you want to review a colleague's pending doc PR in WYSIWYG mode using "hugo". If your colleague's GitHub ID is _"archieb"_ and if the branch used to make the changes is called _"some_doc_changes"_, then clone it thus:

```
git clone --branch some_doc_changes https://github.com/archieb/yugabyte-db.git
```

Start the local webserver on `127.0.0.1` interface by running the following:

```
npm start
```
You can now see the local version of the docs by browsing to [localhost:1313/](http://localhost:1313/).

> **Note #1:** You'll probably find that the first time after the very first `npm start` in a newly-cloned local `git` that you view a page that "hugo" generates, it will look like nonsense. Simply control-C "hugo" and re-issue `npm start`. This sometimes happens, too, after rebasing to the latest commit in the GitHub `yugabyte-db` repo.

> **Note #2:** The URL may be different if the port 1313 is not available. In any case, the URL is printed out on your shell as shown below.

```
Web Server is available at //localhost:1313/ (bind address 0.0.0.0)
Press Ctrl+C to stop
```

> **Note #3:** To start the webserver on some other IP address (in case you want to share the URL of your local docs with someone else), do the following:

   ```
   YB_HUGO_BASE=<YOUR_IP_OR_HOSTNAME> npm start
   ```
   You can now share this link: `http://<YOUR_IP_OR_HOSTNAME>:1313`

### Make your changes

Make sure that your `feature_branch_name` branch is current:

```
git branch
```

If this doesn't respond with `feature_branch_name`, then do this:

```
git checkout feature_branch_name
```

Then make the changes locally and test them by viewing them in a browser.

Once you are satisfied with your changes, use `git status` to list the files that you have modified and created. And if you edited them in a non-Unix environment, you should now ensure that they have Unix-style line endings with a utility like `dos2unix`. You can easily find such a utility with Internet search.

> **Note**: If you commit files with non-Unix-style line endings, then you risk getting spurious conflicts reported when you do `git rebase master`. In particular, you risk confusing the `git` differencing heuristics so that it reports that your file and the same-named file that someone else has changed, and that _does_ have Unix-style line endings, are _entirely_ different—even when human inspection shows only two small non-conflicting changes.

Then commit your changes to your local branch and push these changes to your fork of the [yugabyte-db](https://github.com/yugabyte/yugabyte-db) GitHub repository. Do this by running the following commands.

#### Add files you have made changes to.

```
git add .
```

#### Commit these changes.

```
git commit -m "A useful brief comment about what you did."
```

#### Bring your local git up to date with the yugabyte-db GitHub repository

```
git checkout master
git pull upstream master
git checkout feature_branch_name
git rebase master
```

### Now push your work to your fork

```
git push origin feature_branch_name
```

You can now check that your work arrived safely by visiting this URL and then navigating the file hierarchy:

```
https://github.com/<YOUR_GITHUB_ID>/yugabyte-db/blob/feature_branch_name/docs/
```

### Submit a pull request

The output you see at the O/S prompt following `git push` will tell you which URL to visit in order to create a pull request. We will review your changes, provide feedback when appropriate, and once everything looks good merge your changes into the mainline.

## 2. Advanced usage

In this kind of work, the nature of your documentation change means that you'll need to modify existing [_syntax diagrams_](#syntax-diagram) or add new ones.

> **Note:** The following covers diagram maintenance and creation for the YSQL documentation. The YCQL documentation still uses an old method for this. You must take advice from colleagues if you need to work on YCQL diagrams.

Once you understand this area, modifying existing [_syntax diagrams_](#syntax-diagram) or adding new ones will seem to be very straightforward. However, you must embrace a fairly large mental model. This, in turn, depends on several terms of art. All this is covered in the dedicated section [Creating and maintaining syntax diagrams for YSQL Docs](#3-mental-model-and-glossary-creating-and-maintaining-syntax-diagrams-for-ysql-docs).

Here is a terse step-by-step summary:

### Get set up

1. Download the latest RRDiagram JAR file (`rrdiagram.jar`).  See the section the [_diagram generator_](#diagram-generator).

1. [_Run the generator_](#run-the-generator). This will regenerate every diagram file that is jointly specified by the [_diagram definition file_](#diagram-definition-file) and the set of all [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair). Of course, not all of these newly generated `md` files will differ from their previous versions; `git status` will show you only the ones that actually changed.

### Modify or add content pages or syntax rules

The nature of your documentation change will determine which selection of the following tasks you will need to do and which of these you will do during each local edit-and-review cycle.

- Modify [_humanly typed documentation source_](#humanly-typed-documentation-source) file(s).
- Create new [_humanly typed documentation source_](#humanly-typed-documentation-source) file(s).
- Modify existing [_syntax rule(s)_](#syntax-rule).
- Define new [_syntax rule(s)_](#syntax-rule).
- Create new [_free-standing generated grammar-diagram pair(s)_](#free-standing-generated-grammar-diagram-pair)
- Add [_diagram inclusion HTML_](#diagram-inclusion-HTML) to one or more of the content files that you modified or created.

Very occasionally, you might want to reorganize the hierarchical structure of a part of the overall documentation as the user sees it. (This is the hierarchy that you see and navigate in the left-hand navigation panel.) Such changes involve moving existing  [_humanly typed documentation source_](#humanly-typed-documentation-source) file(s) within the directory tree that starts at the [_content directory_](#content-directory). If you do this, then you must use your favorite editor (a generic plain text editor is best for this purpose) to do manually driven global search and replace to update URL references to moved files at their old locations. However, because you will do this only within the scope of the files that you worked on (most likely, the `/latest/` subtree), you must also establish URL redirects in each moved file to avoid breaking links in other Yugabyte Internet properties, or in third party sites. The _frontmatter_ allows this easily. Here is an example.

```
title: SELECT statement [YSQL]
headerTitle: SELECT
linkTitle: SELECT
description: Use the SELECT statement to retrieve rows of specified columns that meet a given condition from a table.
menu:
  latest:
    identifier: dml_select
    parent: statements
aliases:
  - /latest/api/ysql/commands/dml_select/
isTocNested: true
showAsideToc: true
```
The `aliases` page property allows a list of many URLs. Notice that these are _relative_ to the [_content directory_](#content-directory). The `.md` file in this example used to be here:

```
<your path>/yugabyte-db/docs/content/latest/api/ysql/commands/dml_select.md
```

It was moved to here:

```
<your path>/yugabyte-db/docs/content/latest/api/ysql/the-sql-language/statements/dml_select.md
```

Your specific documentation enhancement will determine if you need only to change existing content pages or to add new ones. You will decide, in turn, if you need to modify any existing [_syntax rules_](#syntax-rule) or to define new ones. And if you do define new ones, you will decide in which files you need to include the "grammar" and "diagram" depictions. (See the section about [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair).)

Follow this general flow on each local editing cycle.

1. Modify content pages, or If necessary create new ones.
1. If necessary, edit the the [_diagram definition file_](#diagram-definition-file).
1. If necessary, create one or more new [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair), using `touch`, on the appropriate directories in the [_syntax resources directory_](#syntax-resources-directory) tree.
1. If necessary, [_run the generator_](#run-the-generator).
1. If necessary, add [_diagram inclusion HTML_](#diagram-inclusion-HTML) to one or more of the content files that you modified.
1. If necessary, add URL references in other content files to link to your new work. For example, you might need to update an `_index.md` file that implements a table of contents to a set of files to which you've just added one.
1. If necessary, add (or update) the `aliases` page property in any  [_humanly typed documentation source_](#humanly-typed-documentation-source) file(s) that you relocated.
1. Manually check the new docs page(s) and index page(s) to make sure that there are no broken links.

> **Note:** There is no tool for link checking within your local git. Broken links are detected when a Yugabyte staff member periodically runs a link checker on the published doc on the Internet. The scope of this check includes _all_ of the Yugabyte properties (for example, the [Blogs site](https://blog.yugabyte.com)). Broken links that this check finds are reported and fixed manually.

## 3. Mental model and glossary: creating and maintaining syntax diagrams for YSQL Docs

This section describes the mental model for the creation and maintenance of [_syntax diagrams_](#syntax-diagram). The description relies on a number of terms of art. These definitions must be in place in order to explain, efficiently and unambiguously, the mental model of how it all works. Each glossary term is italicized, and set up as a link to the term's definition in this essay, whenever it is used to advertise its status as a defined term of art.

The ordering of the glossary terms is insignificant. There’s a fair amount of term-on-term dependency. You therefore have to internalize the whole account by ordinary study. When the mental model that they reflect is firmly in place, you can produce any outcome that you want in this space without step-by-step instructions. In particular, you can base the diagnosis of author-created bugs (for example, broken links) on the mental model.

> **Note:** The account that follows distinguishes between _directory_ and _file_ in the usual way: a _directory_ contains _files_ and/or _directories_. And a _file_ has actual content.
>
> **Note:** the two terms, “grammar” and “syntax”, mean pretty much the same as each other. But one, or the other, of these is used by convention in the spellings and definitions of the terms of art that this glossary defines.
>
> **Note:** Users of the Internet-facing YugabyteDB documentation typically access the sub-corpus that starts here:
>
> [`docs.yugabyte.com/latest/`](https://docs.yugabyte.com/latest/)
>
> Users with more specific requirements will start at `.../stable/` or, maybe, something like `.../v2.1/`. This account assumes that users will work on content only in the `/latest/` subtree.

### syntax rule

- **The formal definition of the grammar of a SQL statement, or a component of a SQL statement**.

Every [_syntax rule_](#syntax-rule) is defined textually in the single [_diagram definition file_](#diagram-definition-file). The set of all these rules is intended to define the entirety of the YSQL grammar—but nothing beyond this. Presently, the definitions of some [_syntax rules_](#syntax-rule) (while these are implemented in the YSQL subsystem of YugabyteDB) remain to be written down.

Sometimes, the grammar of an entire SQL statement can be comfortably described by a single, self-contained [_syntax rule_](#syntax-rule). The [Syntax section](https://docs.yugabyte.com/latest/api/ysql/the-sql-language/statements/txn_commit/#syntax) of the account of the `COMMIT` statement provides an example. More commonly, the grammar of a SQL statement includes references (by name) to the definition(s) of one or more other rule(s). And often such referenced [_syntax rules_](#syntax-rule) are the targets of references from many other [_syntax rules_](#syntax-rule). The complete account of a very flexible SQL statement can end up as a very large closure of multiply referenced rules. [`SELECT`](https://docs.yugabyte.com/latest/api/ysql/the-sql-language/statements/dml_select/#syntax) is the canonical example of complexity. For example, a terminal like [`integer`](https://docs.yugabyte.com/latest/api/ysql/syntax_resources/grammar_diagrams/#integer) can end up as the reference target in very many distinct syntax spots within the total definition of the `SELECT` statement, and of other statements.

A [_syntax rule_](#syntax-rule) is specified using EBNF notation. EBNF stands for “extended Backus–Naur form”. See this [Wikipedia article](https://en.wikipedia.org/wiki/Extended_Backus–Naur_form). Here is an example for the [`PREPARE`](https://docs.yugabyte.com/latest/api/ysql/the-sql-language/statements/perf_prepare/#syntax) statement:

```
prepare_statement ::= 'PREPARE' name [ '(' data_type { ',' data_type } ')' ] 'AS' statement ;
```

> **Note:** When this is presented on the “Grammar” tab in the [_syntax diagram_](#syntax-diagram), it is transformed into the PostgreSQL notation. This is widely used in database documentation. But it is not suitable as input for a program that generate diagrams.

Notice the following:

- The LHS of `::=` is the rule’s name. The RHS is its definition. The definition has three kinds of element:

- _EBNF syntax elements_ like these: `[  ]  (  )  '  {  }  ,`

- _Keywords and punctuation that are part of the grammar that is being defined_. Keywords are conventionally spelled in upper case. Each individual keyword is surrounded by single quotes. This conveys the meaning that whitespace between these, and other elements in the target grammar, is insignificant. Punctuation characters in the target grammar are surrounded by single quotes to distinguish them from punctuation characters in the EBNF grammar.

- _References to other rule names_. These become clickable links in the generated diagrams (but not in the generated grammars). Rule names are spelled in lower case. Notice how underscore is used between the individual English words.

- The single space before the `;` terminator is significant.

### diagram definition file

```
<your path>/yugabyte-db/docs/content/latest/api/ysql/syntax_resources/ysql_grammar.ebnf
```

Located directly on the [_syntax resources directory_](#syntax-resources-directory), this file, uniquely under the whole of the [_ysql directory_](#ysql-directory) is not a `.md` file. And uniquely under the [_syntax resources directory_](#syntax-resources-directory), it is typed up humanly. It holds the definition, written in EBNF notation, of every [_syntax rule_](#syntax-rule) that is processed by the [_diagram generator_](#diagram-generator).

The [_diagram generator_](#diagram-generator) doesn’t care about the order of the rules in the file. But the order is reproduced in the [_grammar diagrams file_](#grammar-diagrams-file-here). This, in turn, reflects decisions made by authors about what makes a convenient reading order. Try to spot what informs the present ordering and insert new rules in a way that respects this. In the limit, insert a new rule at the end as the last new properly-defined rule and before this comment:

```
(* Supporting rules *)
```

### syntax diagram

- **The result, in the published documentation, of a [_syntax rule_](#syntax-rule) that is contained in the [_diagram definition file_](#diagram-definition-file)**.

The [_syntax diagram_](#syntax-diagram) and the [_syntax rule_](#syntax-rule) bear a one-to-one mutual relationship.

The [Syntax section](https://docs.yugabyte.com/latest/api/ysql/the-sql-language/statements/perf_prepare/#syntax) of the account of the [`PREPARE`](https://docs.yugabyte.com/latest/api/ysql/the-sql-language/statements/perf_prepare/) statement provides a short, but sufficient, example. The [_syntax diagram_](#syntax-diagram) appears as a (member of a) tabbed pair which gives the reader the choice to see a [_syntax rule_](#syntax-rule) as either the “Grammar” form (in the syntax used in the PostgreSQL documentation and the documentation for several other databases) or the “Diagram” form (a so-called “railroad diagram” that again is used commonly in the documentation for several other databases).

### docs directory

- **The directory within your local git of the entire documentation source (the [_humanly typed documentation source_](#humanly-typed-documentation-source), the [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair) and the [_diagram definition file_](#diagram-definition-file)) together with the [_supporting doc infrastructure_](#supporting-doc-infrastructure)**:

```
cd <your path>/yugabyte-db/docs
```
You install the [_diagram generator_](#diagram-generator) here. And you stand here when you [_run the generator_](#run-the-generator). You also typically stand here when you start "hugo"; and when you prepare for, and then do, a `git push` to your personal GitHub fork.

### content directory

- **Everything under the [_docs directory_](#docs-directory) that is not [_supporting doc infrastructure_](#supporting-doc-infrastructure)**.

In other words, it holds the entire [_humanly typed documentation source_](#humanly-typed-documentation-source) (not just for YSQL), the [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair) and the [_diagram definition file_](#diagram-definition-file):

```
cd <your path>/yugabyte-db/docs/content
```
"hugo" uses the tree starting at the [_content directory_](#content-directory) to generate the documentation site (under the influence of the [_supporting doc infrastructure_](#supporting-doc-infrastructure)) as a set of static files.

Notice that, when your current directory is the [_docs directory_](#docs-directory), `git status` shows the paths of what it reports starting with the [_content directory_](#content-directory) like this:

```
modified:   content/latest/api/ysql/syntax_resources/grammar_diagrams.md
modified:   content/latest/api/ysql/syntax_resources/ysql_grammar.ebnf
modified:   content/latest/api/ysql/the-sql-language/statements/cmd_do.md
```

Both the `/latest/` and the `/stable/` subtrees are direct children of the [_content directory_](#content-directory).

### ysql directory

- **The directory tree that holds all the [_humanly typed documentation source_](#humanly-typed-documentation-source), the [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair), and the [_diagram definition file_](#diagram-definition-file) that jointly describe the YugabyteDB YSQL subsystem in the /latest/ tree**.

```
cd <your path>/yugabyte-db/docs/content/latest/api/ysql
```

### supporting doc infrastructure

- **Everything on and under the [_docs directory_](#docs-directory) that is not on or under the [_content directory_](#content-directory)**.

This material is not relevant for the present account.

### humanly typed documentation source

- _(with a few exceptions)_ **The set of ".md" files located under the [ysql directory](#ysql-directory)**.

_Singleton exception:_ The [_diagram definition file_](#diagram-definition-file), on the [_syntax resources directory_](#syntax-resources-directory), is a plain text file that is typed up manually.

Exception class: The [_grammar diagrams file_](#grammar-diagrams-file-here) and the [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair). These are all located in the syntax resources directory tree. They are all `.md` files. And none is humanly typed. Do not manually edit any of these.

With just a few on the [_ysql directory_](#ysql-directory) itself, the [_humanly typed documentation source_](#humanly-typed-documentation-source) files are located in directory trees that start at the _ysql directory._ The top-of-section `_index.md` for the YSQL subsystem is among the few [_humanly typed documentation source_](#humanly-typed-documentation-source) files that are located directly on the [_ysql directory_](#ysql-directory).

### syntax resources directory

```
cd <your path>/yugabyte-db/docs/content/latest/api/ysql/syntax_resources
```

With the one exception of the [_diagram definition file_](#diagram-definition-file), every file within the [_syntax resources directory_](#syntax-resources-directory) tree is generated.

### generated documentation source

- **The set of files that the [_diagram generator_](#diagram-generator) produces**.

All are located within the [_syntax resources directory_](#syntax-resources-directory) tree. The set minimally includes the [_grammar diagrams file_](#grammar-diagrams-file-here). It also includes all of the [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair) from this. These will be included within ordinary content `.md` files by URL reference from the [_diagram inclusion HTML_](#diagram-inclusion-HTML) located in the ordinary content `.md` file that wants the [_syntax diagram_](#syntax-diagram).

Notice that there is no garbage collection scheme for unreferenced [_generated documentation source_](#generated-documentation-source) files. Content authors must do this task manually.

### grammar diagrams file ([here](https://docs.yugabyte.com/latest/api/ysql/syntax_resources/grammar_diagrams/#abort))

```
<your path>/yugabyte-db/docs/content/latest/api/ysql/syntax_resources/grammar_diagrams.md
```

This contains every [_syntax diagram_](#syntax-diagram) that is generated from all of the [_syntax rules_](#syntax-rule) that are found in the [_diagram definition file_](#diagram-definition-file).

### generated grammar-diagram pair

- **The Markdown source that produce the result, in the human-readable documentation, that the user sees as a [_syntax diagram_](#syntax-diagram)**.

Use this term when you want to focus in the fact that a [_syntax diagram_](#syntax-diagram) is presented for the user as a tabbed "Grammar" and "Diagram" pair.

Always found in the [_grammar diagrams file_](#grammar-diagrams-file-here) (but, uniquely, not here as a tabbed pair). Optionally found as a [_free-standing generated grammar-diagram pair_](#free-standing-generated-grammar-diagram-pair).

### free-standing generated grammar-diagram pair

- **Such a pair requests the [_diagram generator_](#diagram-generator) to populate them automatically with respectively the "grammar" and the "diagram" presentations of the set of [_syntax rules_](#syntax-rule) that each asks for**.

  They are defined by a pair of `.md` files with names that follow a purpose-designed syntax:
  
```
<rule-name>[, <rule-name>, ...].grammar.md
               ~               .diagram.md
```

Each pair is initially created empty (for example using `touch`) to express the content author’s desire to have a diagram set for a particular [_humanly typed documentation source_](#humanly-typed-documentation-source) file.

Such a pair should be placed in the exact mirror sibling directory, in the [_syntax resources directory_](#syntax-resources-directory) tree, of the directory where the [_humanly typed documentation source_](#humanly-typed-documentation-source) file that wants to include the _specified syntax_ diagram is located.

Here is an example. Suppose that the file `wants-to-include.md` wants to include the [_syntax diagram_](#syntax-diagram) with a rule set denoted by the appropriately spelled identifier `<rule set X>`. And suppose that the [_humanly typed documentation source_](#humanly-typed-documentation-source) file is here:

```
<your path>/yugabyte-db/docs/content/latest/api/ysql                 /dir_1/dir_2/dir_3/wants-to-include.md
```

The white space between `/ysql` and `/dir_1` has been introduced as a device to advertise the convention. It doesn’t exist in the actual file path.

The [_free-standing generated grammar-diagram pair_](#free-standing-generated-grammar-diagram-pair) _must_ be placed here.

```
<your path>/yugabyte-db/docs/content/latest/api/ysql/syntax_resources/dir_1/dir_2/dir_3/<rule set X>.grammar.md
<your path>/yugabyte-db/docs/content/latest/api/ysql/syntax_resources/dir_1/dir_2/dir_3/<rule set X>.diagram.md
```

Suppose that a [_syntax rule_](#syntax-rule) includes a reference to another [_syntax rule_](#syntax-rule). If the referenced [_syntax rule_](#syntax-rule) is included (by virtue of the name of the _diagram-grammar file pair_) in the same [_syntax diagram set_](#syntax-diagram-set), then the name of the [_syntax rule_](#syntax-rule) in the referring [_syntax diagram_](#syntax-diagram) becomes a link to the [_syntax rule_](#syntax-rule) in that same [_syntax diagram set_](#syntax-diagram-set). Otherwise the generated link target of the referring rule is within the [_grammar diagrams file_](#grammar-diagrams-file-here). The way that this link is spelled depends on the location, within the [_ysql directory_](#ysql-directory) tree, of the `.md` file that includes the generated [_syntax diagram_](#syntax-diagram).

If you don’t follow this rule, then (as long as you specify the right path to the [_free-standing generated grammar-diagram pair_](#free-standing-generated-grammar-diagram-pair) in the [_diagram inclusion HTML_](#diagram-inclusion-HTML), you _will_ get the diagram in the `wants-to-include.md`, just as you want it. But the links to diagrams in the [_grammar diagrams file_](#grammar-diagrams-file-here) for rules that are not defined in the same included diagram will be broken. You should therefore always check manually that these links work when you first confirm that you see the generated [_syntax diagram_](#syntax-diagram) that you want at the location that you want it. And you should check again just before creating a Pull Request.

### syntax diagram set

- **The set of [_syntax diagrams_](#syntax-diagram) specified by spelling `<rule-name>[ , ... ]` in the first part of the filenames of the [_free-standing generated grammar-diagram pair_](#free-standing-generated-grammar-diagram-pair)**.

### diagram inclusion HTML

- **The HTML snippet that you include, and edit, in a [_humanly typed documentation source_](#humanly-typed-documentation-source) file in which you want to include a [_free-standing generated grammar-diagram pair_](#free-standing-generated-grammar-diagram-pair)**.

Consider this example:

```
<your path>/yugabyte-db/docs/content/latest/api/ysql/dir_1/dir_2/dir_3/wants-to-include.md
```

You must include this boilerplate text in `wants-to-include.md` at the location in this file where you want the [_syntax diagram_](#syntax-diagram) to be seen:

```
<ul class=“nav nav-tabs nav-tabs-yb”>
  <li >
    <a href=“#grammar” class=“nav-link active” id=“grammar-tab” data-toggle=“tab” role=“tab” aria-controls=“grammar” aria-selected=“true”>
      <i class=“fas fa-file-alt” aria-hidden=“true”></i>
      Grammar
    </a>
  </li>
  <li>
    <a href=“#diagram” class=“nav-link” id=“diagram-tab” data-toggle=“tab” role=“tab” aria-controls=“diagram” aria-selected=“false”>
      <i class=“fas fa-project-diagram” aria-hidden=“true”></i>
      Diagram
    </a>
  </li>
</ul>

<div class=“tab-content”>
  <div id=“grammar” class=“tab-pane fade show active” role=“tabpanel” aria-labelledby=“grammar-tab”>
    {{% includeMarkdown “../../../syntax_resources/dir_1/dir_2/dir_3/<rule set X>.grammar.md” /%}}
  </div>
  <div id=“diagram” class=“tab-pane fade” role=“tabpanel” aria-labelledby=“diagram-tab”>
    {{% includeMarkdown “../../../syntax_resources/dir_1/dir_2/dir_3/<rule set X>.diagram.md” /%}}
  </div>
</div>
```

You take responsibility for spelling this

```
../../../syntax_resources/dir_1/dir_2/dir_3/<rule set X>.grammar.md
```

and this:

```
../../../syntax_resources/dir_1/dir_2/dir_3/<rule set X>.diagram.md
```

properly. The rest is entirely boilerplate. You musn’t touch it. The construct `../../../` must climb up the proper number of levels (three in this example) to reach the parent [_ysql directory_](#ysql-directory) from `wants-to-include.md`.

### diagram generator

- **The program that reads the [_diagram definition file_](#diagram-definition-file) and that repopulates the [_grammar diagrams file_](#grammar-diagrams-file-here) and the set of existing [_free-standing generated grammar-diagram pairs_](#free-standing-generated-grammar-diagram-pair) for every rule that the [_diagram definition file_](#diagram-definition-file) defines**.

It's implemented by `rrdiagram.jar` on the [_docs directory_](#docs-directory).

Here are the download instructions. You need to do this just once in a newly created local git (typically created from one’s personal fork of the master YB repo).

```
cd <your path>/yugabyte-db/docs/

wget $(curl -s https://api.github.com/repos/Yugabyte/RRDiagram/releases/latest \
       | grep browser_download_url | cut -d \" -f 4)
```

Use `ls rrdiagram.jar` to check that it is now present. (It isn't shown by "git status" 'cos it's explicitly excluded.)

### run the generator

- **first "cd..." then "java..."**

Specifically:

```
cd <your path>/yugabyte-db/docs
java -jar rrdiagram.jar content/latest/api/ysql/syntax_resources/ysql_grammar.ebnf \
  content/latest/api/ysql/syntax_resources/
```

This will (re)generate _all_ of the files that it ought to. You can run this at any time. In the worst case, a typing error somewhere, especially in the [_diagram inclusion HTML_](#diagram-inclusion-HTML), can crash "hugo", resulting in the notorious blanked out screen in the browser.

> **Note:** To see help, run `java -jar rrdiagram.jar` (without arguments).

### About the redundant use of ( ... )

In at least one case, the use of semantically redundant notation in the [_diagram definition file_](#diagram-definition-file) conveys the author’s intent about how the diagram should be constructed. Here’s an example of semantically significant use of `( ... )` in algebra:

&nbsp;&nbsp;`(a/b) - c` differs in meaning from `a/(b - c)`

And while the rules do give `a/b - c` an unambiguous meaning (advertised by the absence of space surrounding the `/` operator), it's generally considered unhelpful to write this, thereby forcing the reader to rehearse the rules in order to discern the meaning. Rather, the semantically redundant orthography `(a/b) - c` is preferred.

In EBNF, `( ... )` plays a similar role as it does in algebra. In some situations, just which subset of items is grouped changes the meaning. But in some situations (like `a + b + c` in algebra) the use of `( ... )` conveys no semantics. Look for this at the end of the [_diagram definition file_](#diagram-definition-file):

```
(*
  Notice that the "demo-2" rule uses ( ... ) redundantly.
  The two rules are semantically identical but they produce the different diagrams.
  The diagrams do express the same meaning but they are drawn using different conventions.
  Uncomment the two rule definitions and look at the end of the grammar diagrams file to
  see the two semantically equivalent, but differently drawn, diagrams.

  Make sure that you comment these out again before creating a Pull request.
*)

(*
demo-1-irrelevant-for-ysql-syntax ::= ( a 'K' b { ',' a 'K' b } ) ;

demo-2-irrelevant-for-ysql-syntax ::= ( ( a 'K' b ) { ',' ( a 'K' b ) } ) ;
*)
```

### Example use case

Suppose that this [_humanly typed documentation source_](#humanly-typed-documentation-source) file:

```
<your path>/yugabyte-db/docs/content/latest/api/ysql/exprs/window_functions/window-definition.md
```
wants to include a [_syntax diagram set_](#syntax-diagram-set) with these [_syntax rules_](#syntax-rule):

> `frame_clause`, `frame_bounds`, and `frame_bound`

_Step 1:_ If they all already exist in the [_diagram definition file_](#diagram-definition-file), then go to _Step 2_. Else, type up any rules that don’t yet exist.

_Step 2:_ Create these empty files (for example, with `touch`).

```
<your path>/yugabyte-db/docs/content/latest/api/ysql/syntax_resources/exprs/window_functions/frame_clause,frame_bounds,frame_bound.diagram.md

<your path>/yugabyte-db/docs/content/latest/api/ysql/syntax_resources/exprs/window_functions/frame_clause,frame_bounds,frame_bound.grammar.md
```

_Step 3:_ Copy a reliable example of the [_diagram inclusion HTML_](#diagram-inclusion-HTML) and paste it into `window-definition.md` at the location where you want the syntax diagram set to appear. Edit the bolded text shown above appropriately.

_Step 4:_ [_Run the generator_](#run-the-generator).

_Step 5:_ Make sure that you inspect the result immediately in the "hugo" rendering of the [_humanly typed documentation source_](#humanly-typed-documentation-source) into which you just included the [_diagram inclusion HTML_](#diagram-inclusion-HTML).
