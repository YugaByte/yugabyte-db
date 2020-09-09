// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.models;

import io.ebean.*;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.validation.Constraints;
import play.libs.Json;

import javax.persistence.Column;
import javax.persistence.EmbeddedId;
import javax.persistence.Entity;
import java.util.List;

@Entity
public class PriceComponent extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(PriceComponent.class);

  @EmbeddedId
  @Constraints.Required
  private PriceComponentKey idKey;

  public String getProviderCode() {
    return this.idKey.providerCode;
  }

  public String getRegionCode() {
    return this.idKey.regionCode;
  }

  public String getComponentCode() {
    return this.idKey.componentCode;
  }

  @Column(nullable = false, columnDefinition = "TEXT")
  private String priceDetailsJson;

  public void setPriceDetails(PriceDetails details) {
    this.priceDetailsJson = Json.stringify(Json.toJson(details));
    this.save();
  }

  public PriceDetails priceDetails = new PriceDetails();

  private static final Finder<PriceComponentKey, PriceComponent> find =
    new Finder<PriceComponentKey, PriceComponent>(PriceComponent.class) {};

  /**
   * Get a single specified pricing component for a given provider and region.
   *
   * @param providerCode The cloud provider that the pricing component is in.
   * @param regionCode The region that the pricing component is in.
   * @param componentCode The pricing component's code.
   * @return The uniquely matching pricing component.
   */
  public static PriceComponent get(String providerCode, String regionCode, String componentCode) {
    PriceComponentKey pcKey = PriceComponentKey.create(providerCode, regionCode, componentCode);
    PriceComponent pc = PriceComponent.find.byId(pcKey);
    if (pc != null) {
      pc.priceDetails = new PriceDetails();
      if (pc.priceDetailsJson != null && !pc.priceDetailsJson.isEmpty()) {
        pc.priceDetails = Json.fromJson(Json.parse(pc.priceDetailsJson), PriceDetails.class);
      }
    }
    return pc;
  }

  /**
   * Query helper to find pricing components for a given cloud provider.
   *
   * @param provider The cloud provider to find pricing components of.
   * @return A list of pricing components in the cloud provider.
   */
  public static List<PriceComponent> findByProvider(Provider provider) {
    return PriceComponent.find.query().where()
        .eq("provider_code", provider.code)
        .findList();
  }

  /**
   * Query helper to find pricing components for a given region in a given cloud provider.
   *
   * @param provider The cloud provider to find pricing components of.
   * @param region The region to find pricing components of.
   * @return A list of pricing components in the cloud provider's region.
   */
  public static List<PriceComponent> findByRegion(Provider provider, Region region) {
    return PriceComponent.find.query().where()
        .eq("provider_code", provider.code)
        .eq("region_code", region.code)
        .findList();
  }

  /**
   * Create or update a pricing component.
   *
   * @param providerCode Cloud provider that the pricing component belongs to.
   * @param regionCode Region in the cloud provider that the pricing component belongs to.
   * @param componentCode The identifying code for the pricing component. Must be unique within
   *                      the region.
   * @param priceDetails The pricing details of the component.
   * @return The newly created/updated pricing component.
   */
  public static void upsert(String providerCode, String regionCode, String componentCode,
                            PriceDetails priceDetails) {
    PriceComponent component = PriceComponent.get(providerCode, regionCode, componentCode);
    if (component == null) {
      component = new PriceComponent();
      component.idKey = PriceComponentKey.create(providerCode, regionCode, componentCode);
    }
    PriceDetails details = priceDetails == null ? new PriceDetails() : priceDetails;
    component.setPriceDetails(details);
  }

  /**
   * The actual details of the pricing component.
   */
  public static class PriceDetails {

    // The unit on which the 'pricePerUnit' is based.
    public enum Unit {
      Hours,
      GBMonth,
      PIOPMonth
    }

    // The price currency. Note that the case here matters as it matches AWS output.
    public enum Currency {
      USD
    }

    // The unit.
    public Unit unit;

    // Price per unit.
    public double pricePerUnit;

    // Price per hour. Derived from unit (might be per hour or per month).
    public double pricePerHour;

    // Price per day (24 hour day). Derived from unit (might be per hour or per month).
    public double pricePerDay;

    // Price per month (30 day month). Derived from unit (might be per hour or per month).
    public double pricePerMonth;

    // Currency.
    public Currency currency;

    // Keeping these around for now as they seem useful.
    public String effectiveDate;
    public String description;

    public void setUnitFromString(String unit) {
      switch (unit.toUpperCase()) {
        case "GB-MO":
        case "GBMONTH":
          this.unit = Unit.GBMonth;
          break;
        case "HRS":
        case "HOURS":
          this.unit = Unit.Hours;
          break;
        case "IOPS-MO":
          this.unit = Unit.PIOPMonth;
          break;
        default:
          LOG.error("Invalid price unit provided: " + unit);
          break;
      }
    }

  }

}
