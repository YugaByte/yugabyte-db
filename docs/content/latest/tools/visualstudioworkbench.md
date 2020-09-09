---
title: Use Visual Studio Code with YugabyteDB YCQL
headerTitle: Visual Studio Code
linkTitle: Visual Studio Code
description: Configure Visual Studio Code to work with YCQL.
menu:
  latest:
    identifier: visualstudioworkbench
    parent: tools
    weight: 2760
isTocNested: true
showAsideToc: true
---

## Introduction

In this tutorial, you will show how to install the [Apache Cassandra Workbench](https://marketplace.visualstudio.com/items?itemName=kdcro101.vscode-cassandra#quick-start) extension in Visual Studio Code and configure a connection.

## Install the VS Code extension

In this tutorial, you will show how to install the [Apache Cassandra Workbench](https://marketplace.visualstudio.com/items?itemName=kdcro101.vscode-cassandra#quick-start) extension in Visual Studio Code and configure a connection.

Open Visual Studio Code (you can download it from https://code.visualstudio.com for Windows, Mac or Linux) and press `Control + P`.

![VSCode Quick Open](/images/develop/tools/vscodeworkbench/vscode_control_p.png)

Paste the following command and press enter.

```
ext install kdcro101.vscode-cassandra
```

This will install the extension, but you will need to configure the connection details of the clusters, so go to the next step and configure connections.

## Create a configuration

Click in cloud icon in the left bar in VSCode to show Cassandra Workbench.

![Open Cassandra Workbench](/images/develop/tools/vscodeworkbench/cloudicon.png)

Press Control + Shift + P to open the actions input and type:

```
Cassandra Workbench: Generate configuration
```

This will generate .cassandraWorkbench.jsonc configuration file.

Open and configure adding cluster as you need with connections informations: YugabyteDB ContactPoints, Port and Authentication Details (if you using Password Authenticator)

```
// name must be unique!
[
    // AllowAllAuthenticator
    {
        "name": "Cluster AllowAllAuthenticator",
        "contactPoints": ["127.0.0.1"]
    },
    //PasswordAuthenticator
    {
        "name": "Cluster PasswordAuthenticator",
        "contactPoints": ["127.0.0.1"],
        "authProvider": {
            "class": "PasswordAuthenticator",
            "username": "yourUsername",
            "password": "yourPassword"
        }
    }
]

```

## Enjoy

Now you are ready to explore YCQL schema and data by simply double-clicking on the connection name.

![EDITOR UI](/images/develop/tools/vscodeworkbench/editor-ui.png)

Details in  [Cassandra Workbench for Visual Studio Code](https://marketplace.visualstudio.com/items?itemName=kdcro101.vscode-cassandra).
