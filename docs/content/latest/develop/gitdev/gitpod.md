---
title: Gitpod
linkTitle: Gitpod
description: Ready to code Gitpod integrated dev environment
menu:
  latest:
    identifier: gitpod
    parent: gitdev
    weight: 592
isTocNested: true
showAsideToc: true
---

Use the [Gitpod](https://www.gitpod.io) workspaces to provision an instant development environment with a pre-configured YugabyteDB.

Gitpod is a configurable ready to code cloud development environment accessible via a browser. A Gitpod workspace includes everything developers need to develop for a specific repository, including the `vscode` or `theia` editing experience, common languages, tools, and utilities. Instantly it sets up a cloud-hosted, containerized, and customizable vscode/theia environment.

Follow the steps below to set up a Gitpod workspace environment with a pre-configured YugabyteDB. For details on Gitpod workspaces, see the [Gitpod documentation](https://www.gitpod.io/docs/).

## Requirements ⏳
Gitpod doesn't require anything in your local workstation other than a code editor and git cli. Much of the development happens in the cloud through a web browser.

## Getting Started with a boot app
You can find the source at [Spring Boot todo on GitHub](https://github.com/srinivasa-vasu/todo).

### Initialize the base project structure
Spring todo is a Java Spring Boot reactive app. However, the steps to go through the Gitpod experience are agnostic of the language/framework. A quick way to get started with a spring boot app is via the [Spring Initializer](https://start.spring.io). Generate the base project structure with Webflux, Flyway, and R2DBC dependencies.

![set-up the base project abstract](/images/develop/gitdev/gitpod/init-sb.png)

### Complete the CRUD APIs
Complete the todo-service to handle 'GET', 'POST', 'PUT', and 'DELETE' API requests.

![complete the api endpoints](/images/develop/gitdev/gitpod/complete-api.png)

{{< note title="Note" >}}
📌 It uses non-blocking reactive APIs to connect to the YugabyteDB.
{{< /note >}}

## Initialize Gitpod
To get started quickly, you can use the universal image [pre-built containers](https://www.gitpod.io/docs/quickstart) or a language-specific image. It can be further customized to fit your needs either by extending them or by creating a new one. A simple click provisions the entire development environment in the cloud with an integrated powerful vscode/theia editor. The entire config to set up the development environment lives in the same source code repository. Let's go through the steps to set up the Gitpod environment.

### Setting up the Gitpod environment
Gitpod workspace environment is initialized for a specific repository by invoking `https://gitpod.io/#[REPO_URL]`

![initalize the workspace environment](/images/develop/gitdev/gitpod/init-workspace.png)

You can either use a universal image with pre-configured libraries and commonly used utilities or a language specific image. Let's create the integrated YugabuteDB workspace environment by customizing the base universal image.

To initialize the workspace environment, 
- create `.gitpod.yml` file at the root of the source repo
- create `.gitpodcontainer` folder at the root of the source repo to keep the customized `Dockerfile`


### Customize the Gitpod environment
You need to customize the default universal image to include the YugabyteDB binary. This is done by defining your own `Dockerfile` at `.gitpodcontainer/Dockerfile`.

```docker
# default universal image
FROM gitpod/workspace-full

ARG YB_VERSION=2.7.1.1
ARG ROLE=gitpod

USER root

RUN apt-get update && apt-get install -y \
	netcat --no-install-recommends
# download and initialize the file structure
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

USER $ROLE
```

The following lines of code are to write the app-specific database-related info to a local file that will be run during the container initialization phase.

``` docker
ENV STORE=/var/ybdp
ENV LISTEN=127.0.0.1
ENV PORT=5433

RUN echo "CREATE DATABASE todo;" > $STORE/init-db.sql \
	&& echo "CREATE USER todo WITH PASSWORD 'todo';" >> $STORE/init-db.sql \
	&& echo "GRANT ALL PRIVILEGES ON DATABASE todo TO todo;" >> $STORE/init-db.sql \
	&& echo '\\c todo;' >> $STORE/init-db.sql \
	&& echo "CREATE EXTENSION IF NOT EXISTS \"uuid-ossp\";" >> $STORE/init-db.sql
```

To initialize the workspace launch environment, customize the `.gitpod.yml` file as,
```yml
# Refer the customized docker image
image:
  file: .gitpodcontainer/Dockerfile

# Run the app and db related jobs
# - run the db post-init script
# - start the DB instance
# - run the db app specific init script
# - build and run the app process
tasks:
  - name: db-run
    before: /usr/local/yugabyte/bin/post_install.sh
    init: |
      yugabyted start --base_dir=$STORE/ybd1 --listen=$LISTEN && \
      [[ ! -f $STORE/.init-db.sql.completed ]] &&  { for i in {1..10}; do (nc -vz $LISTEN $PORT >/dev/null 2>&1); [[ $? -eq 0 ]] &&  { ysqlsh -f $STORE/init-db.sql; touch $STORE/.init-db.sql.completed; break; } || sleep $i; done } && \
      [[ ! -f $STORE/.init-db.sql.completed ]] && echo 'YugabyteDB is not running!'
  - name: app-run
    init: gradle clean build -x test
    command: java -jar build/libs/*.jar
```

If you launch the workspaces environment with the above-updated spec, the development environment gets provisioned with a running YugabyteDB instance. It opens two terminals, one terminal to run the DB task and the other one to compile and run the boot app.

![install YugabyteDB](/images/develop/gitdev/gitpod/install-yb.gif)

{{< note title="Note" >}}
📌 Gitpod provisions a fully integrated ready to code cloud-native development environment with an automated port forwarding to develop, build, and test applications right from the browser tab.
{{< /note >}}

![workspace environment](/images/develop/gitdev/gitpod/workspace.png)

## ⌛ Summary
Gitpod provides fully automated, pre-configured, and consistent development environments that improve the productivity of distributed teams.

![complete dev environment](/images/develop/gitdev/gitpod/complete-dev.png)

