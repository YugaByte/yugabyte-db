---
title: Codespaces
linkTitle: Codespaces
description: GitHub Codespaces
menu:
  latest:
    identifier: codespace
    parent: gitdev
    weight: 582
isTocNested: true
showAsideToc: true
---

Use the [GitHub Codespaces](https://github.com/features/codespaces) to provision an instant development environment with a pre-configured YugabyteDB.

Codespaces is a configurable cloud development environment accessible via a browser or through a local vscode editor. A codespace includes everything developers need to develop for a specific repository, including the vscode editing experience, common languages, tools, and utilities. Instantly it sets up a cloud-hosted, containerized, and customizable vscode environment.

Follow the steps below to set up a codespace environment with a pre-configured YugabyteDB. For details on GitHub Codespaces, see the [GitHub Codespaces documentation](https://docs.github.com/en/codespaces).

## Requirements
This doesn't require anything in your local workstation other than a code editor and git cli. Much of the development happens in the cloud through a web browser though you have the option to use vscode locally.

## Getting Started with a boot app
You can find the source at [Spring Boot todo on GitHub](https://github.com/srinivasa-vasu/todo).

### Initialize the base project structure
This is a Java Spring Boot reactive app. However, the steps to go through the Codespaces experience are agnostic of the language/framework. A quick way to get started with a spring boot app is via the [Spring Initializer](https://start.spring.io). Generate the base project structure with Webflux, Flyway, and R2DBC dependencies.

![set-up the base project abstract](/images/develop/gitdev/codespace/init-sb.png)

### Complete the CRUD APIs
Complete the todo-service to handle 'GET', 'POST', 'PUT', and 'DELETE' API requests.

![complete the api endpoints](/images/develop/gitdev/codespace/complete-api.png)

{{< note title="Note" >}}
It uses non-blocking reactive APIs to connect to the YugabyteDB.
{{< /note >}}

## Initialize Codespaces
To get started quickly, you can use one of the appropriate readily available [pre-built containers](https://github.com/microsoft/vscode-dev-containers/tree/main/containers). It can be further customized to fit your needs either by extending them or by creating a new one. A simple click provisions the entire development environment in the cloud with an integrated powerful vscode editor in a container form factor. The entire config to set up the development environment lives in the same source code repository. Let's go through the steps to set up the codespaces environment.

### Setting up the Codespace environment
As of writing this, Codespaces is still a beta feature. If this feature is enabled for your GitHub account, then the codespace environment is initialized at [GitHub Codespaces](https://github.com/codespaces).
![initalize the codespace environment](/images/develop/gitdev/codespace/init-codespace.png)

If you don't keep any codespace-specific spec in the source repo, then on clicking `Create codespace`, a default [dev environment](https://github.com/microsoft/vscode-dev-containers/tree/main/containers/codespaces-linux) gets provisioned with `codespaces-linux` container. It is a universal image with pre-built language-specific libraries and commonly used utilities. As you need a dev environment with an integrated YugabyteDB, you have to customize the universal image. If the default conventions provided by the codespaces are not sufficient, then you can provide our own configuration.

To initialize the codespace environment, open the source code in a local `vscode` editor. Install the following extensions,
- Remote - Containers
- GitHub Codespaces

In the command palette (fn+F1), type `Remote-containers: Add` and select `Add Development Container Configuration files` and then type `Ubuntu` in the next prompt,
![initialize the remote containers](/images/develop/gitdev/codespace/find-container.png)

It initializes a `.devcontainer` folder with a JSON metadata file at the root of the source repo. `devcontainer.json` has the info to provision the development environment with all the necessary tools and runtime stack. 

### Customize the Codespace environment
You need to customize the default universal image to include the YugabyteDB binary. This is done by defining your own `Dockerfile`.

```docker
ARG VERSION
FROM mcr.microsoft.com/vscode/devcontainers/universal:$VERSION

ARG YB_VERSION
ARG ROLE

USER root

RUN apt-get update && export DEBIAN_FRONTEND=noninteractive && \
	apt-get install -y netcat --no-install-recommends

RUN curl -sSLo ./yugabyte.tar.gz https://downloads.yugabyte.com/yugabyte-${YB_VERSION}-linux.tar.gz \
	&& mkdir yugabyte \
    && tar -xvf yugabyte.tar.gz -C yugabyte --strip-components=1 \
    && mv ./yugabyte /usr/local/ \
    && ln -s /usr/local/yugabyte/bin/yugabyted /usr/local/bin/yugabyted \
    && ln -s /usr/local/yugabyte/bin/ysqlsh /usr/local/bin/ysqlsh \
    && chmod +x /usr/local/bin/yugabyted \
    && chmod +x /usr/local/bin/ysqlsh \
    && rm ./yugabyte.tar.gz

RUN mkdir -p /var/ybdp \
	&& chown -R $ROLE:$ROLE /var/ybdp \
	&& chown -R $ROLE:$ROLE /usr/local/yugabyte
```

Update the devcontainer.json spec to refer your customized file,
```json
{
  "name": "Yugabyte Codespace",
  "build": {
    "dockerfile": "Dockerfile",
    "args": {
      "VERSION": "focal",
      "YB_VERSION": "2.7.1.1",
      "ROLE": "codespace"
    }
  }
```

The following lines of code are to initialize the YugabyteDB with an app-specific database.

``` docker
RUN echo "CREATE DATABASE todo;" > $STORE/init-db.sql \
	&& echo "CREATE USER todo WITH PASSWORD 'todo';" >> $STORE/init-db.sql \
	&& echo "GRANT ALL PRIVILEGES ON DATABASE todo TO todo;" >> $STORE/init-db.sql \
	&& echo '\\c todo;' >> $STORE/init-db.sql \
	&& echo "CREATE EXTENSION IF NOT EXISTS \"uuid-ossp\";" >> $STORE/init-db.sql

RUN echo "/usr/local/yugabyte/bin/post_install.sh 2>&1" >> ~/.bashrc
RUN echo "yugabyted start --base_dir=$STORE/ybd1 --listen=$LISTEN" >> ~/.bashrc
RUN echo "[[ ! -f $STORE/.init-db.sql.completed ]] && " \
	"{ for i in {1..10}; do (nc -vz $LISTEN $PORT >/dev/null 2>&1); [[ \$? -eq 0 ]] && "\
	"{ ysqlsh -f $STORE/init-db.sql; touch $STORE/.init-db.sql.completed; break; } || sleep \$i; done }" >> ~/.bashrc
RUN echo "[[ ! -f $STORE/.init-db.sql.completed ]] && echo 'YugabyteDB is not running!'" >> ~/.bashrc
```

If you run the `Create codespace` with the above-updated spec, the development environment gets provisioned with a running YugabyteDB instance.

![install YugabyteDB](/images/develop/gitdev/codespace/install-yb.gif)

{{< note title="Note" >}}
Fully provisioned, integrated development environment with an automated port forwarding to build and test the application via a browser tab.
{{< /note >}}


## Summary
GitHub codespaces provides the automated, pre-configured, and consistent development environments that improves the productivity of distributed teams.

![complete dev environment](/images/develop/gitdev/codespace/complete-dev.png)