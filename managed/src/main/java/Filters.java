// Copyright (c) YugaByte, Inc.

import com.google.inject.Inject;
import play.mvc.EssentialFilter;
import play.filters.cors.CORSFilter;
import play.filters.csrf.CSRFFilter;
import play.http.HttpFilters;

import java.util.ArrayList;
import java.util.List;

public class Filters implements HttpFilters {

  @Inject
  CORSFilter corsFilter;

  @Inject
  CSRFFilter csrfFilter;

  public List<EssentialFilter> getFilters() {
    ArrayList<EssentialFilter> filters = new ArrayList<EssentialFilter>();
    filters.add(csrfFilter.asJava());
    filters.add(corsFilter.asJava());

    return filters;
  }
}
