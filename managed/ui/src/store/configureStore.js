// Copyright (c) YugaByte, Inc.

if (
  process.env.NODE_ENV === 'production' ||
  (window.location && window.location.hostname !== 'localhost')
) {
  module.exports = require('./configureStore.prod');
} else {
  module.exports = require('./configureStore.dev');
}
