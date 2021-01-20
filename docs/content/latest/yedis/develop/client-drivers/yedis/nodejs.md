---
title: Build a YugabyteDB application using NodeJS and YEDIS
headerTitle: Build an application using NodeJS
linkTitle: NodeJS
description: Use NodeJS to build a YugabyteDB application that interacts with YEDIS
aliases:
  - /latest/yedis/develop/client-drivers/nodejs
menu:
  latest:
    identifier: client-drivers-yedis-nodejs
    parent: develop-yedis
isTocNested: true
showAsideToc: true
---

## Installation

Install the NodeJS driver using the following command.

```sh
$ npm install redis
```

## Working Example

### Prerequisites

This tutorial assumes that you have:

- installed YugabyteDB, created a universe, and are able to interact with it using the Redis shell. If not, follow the steps in [Quick start](../../../../quick-start/).
- installed a recent version of `node`. If not, you can find install instructions [here](https://nodejs.org/en/download/).

### Write the HelloWorld NodeJS application

Create a file `yb-redis-helloworld.js` and add the following content to it.

```js
const redis = require("redis")
const client = redis.createClient();

client.on("error", function (err) {
    console.log("Error " + err);
});

// Insert the user profile.
const userid = 1
client.hmset(userid, ["name", "John", "age", "35", "language", "NodeJS"], redis.print);

// Query the user profile.
client.hmget(userid, ["name", "age", "language"], redis.print);

// Close the client.
client.quit(function (err, res) {
    console.log('Exiting from quit command.');
});
```

### Run the application

To run the application, type the following:

```sh
$ node yb-redis-helloworld.js
```

You should see the following output.

```
Reply: OK
Reply: John,35,NodeJS
Exiting from quit command.
```
