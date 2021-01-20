package com.yugabyte.yw.cloud.aws;

import com.google.inject.AbstractModule;
import com.google.inject.multibindings.MapBinder;
import com.yugabyte.yw.cloud.CloudAPI;

public class AWSCloudModule extends AbstractModule {
  @Override
  protected void configure() {
    MapBinder<String, CloudAPI> mapBinder =
      MapBinder.newMapBinder(binder(), String.class, CloudAPI.class);
    mapBinder.addBinding("aws").to(AWSCloudImpl.class);
  }
}
