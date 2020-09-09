// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.models;

import java.util.Date;
import java.util.HashSet;
import java.util.Set;
import java.util.UUID;
import java.util.List;
import java.util.Arrays;
import java.util.stream.Collectors;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.GeneratedValue;
import javax.persistence.GenerationType;
import javax.persistence.Id;
import javax.persistence.SequenceGenerator;

import com.yugabyte.yw.forms.UniverseDefinitionTaskParams.Cluster;

import org.joda.time.DateTime;
import org.mindrot.jbcrypt.BCrypt;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import io.ebean.annotation.DbJson;
import io.ebean.*;
import com.fasterxml.jackson.annotation.JsonFormat;
import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.google.common.base.Joiner;

import play.data.validation.Constraints;
import play.libs.Json;

import static com.yugabyte.yw.models.helpers.CommonUtils.deepMerge;

@Entity
public class Customer extends Model {

  public static final Logger LOG = LoggerFactory.getLogger(Customer.class);
  // A globally unique UUID for the customer.
  @Column(nullable = false, unique = true)
  public UUID uuid = UUID.randomUUID();
  public void setUuid(UUID uuid) { this.uuid = uuid;}

  // An auto incrementing, user-friendly id for the customer. Used to compose a db prefix. Currently
  // it is assumed that there is a single instance of the db. The id space for this field may have
  // to be partitioned in case the db is being sharded.
  @Id
  @SequenceGenerator(name="customer_id_seq", sequenceName="customer_id_seq", allocationSize=1)
  @GeneratedValue(strategy = GenerationType.SEQUENCE, generator="customer_id_seq")  private Long id;
  public Long getCustomerId() { return id; }

  @Column(length = 15, nullable = false)
  @Constraints.Required
  public String code;

  @Column(length = 256, nullable = false)
  @Constraints.Required
  @Constraints.MinLength(3)
  public String name;

  @Column(nullable = false)
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  public Date creationDate;

  @Column(nullable = true, columnDefinition = "TEXT")
  private JsonNode features;

  @Column(columnDefinition = "TEXT", nullable = false)
  private String universeUUIDs = "";

  public synchronized void addUniverseUUID(UUID universeUUID) {
    Set<UUID> universes = getUniverseUUIDs();
    universes.add(universeUUID);
    universeUUIDs = Joiner.on(",").join(universes);
    LOG.debug("New universe list for customer [" + name + "] : " + universeUUIDs);
  }

  public synchronized void removeUniverseUUID(UUID universeUUID) {
    Set<UUID> universes = getUniverseUUIDs();
    universes.remove(universeUUID);
    universeUUIDs = Joiner.on(",").join(universes);
    LOG.debug("New universe list for customer [" + name + "] : " + universeUUIDs);
  }

  public Set<UUID> getUniverseUUIDs() {
    Set<UUID> uuids = new HashSet<UUID>();
    if (!universeUUIDs.isEmpty()) {
      List<String> ids = Arrays.asList(universeUUIDs.split(","));
      for (String id : ids) {
        uuids.add(UUID.fromString(id));
      }
    }
    return uuids;
  }

  @JsonIgnore
  public Set<Universe> getUniverses() {
    if (getUniverseUUIDs().isEmpty()) {
      return new HashSet<>();
    }
    return Universe.get(getUniverseUUIDs());
  }

  @JsonIgnore
  public Set<Universe> getUniversesForProvider(UUID providerUUID) {
    Set<Universe> universesInProvider = getUniverses()
        .stream().filter(u -> checkClusterInProvider(u, providerUUID))
        .collect(Collectors.toSet());
    return universesInProvider;
  }

  private boolean checkClusterInProvider(Universe universe, UUID providerUUID) {
    for (Cluster cluster : universe.getUniverseDetails().clusters) {
      if (cluster.userIntent.provider.equals(providerUUID.toString())) {
        return true;
      }
    }
    return false;
  }

  public static final Finder<UUID, Customer> find = new Finder<UUID, Customer>(Customer.class) {
  };

  public static Customer get(UUID customerUUID) {
    return find.query().where().eq("uuid", customerUUID).findOne();
  }

  public static Customer get(long id) {
    return find.query().where().idEq(String.valueOf(id)).findOne();
  }

  public static List<Customer> getAll() {
    return find.query().findList();
  }

  public Customer() {
    this.creationDate = new Date();
  }

  /**
   * Create new customer, we encrypt the password before we store it in the DB
   *
   * @param name
   * @param email
   * @param password
   * @return Newly Created Customer
   */
  public static Customer create(String code, String name) {
    Customer cust = new Customer();
    cust.code = code;
    cust.name = name;
    cust.creationDate = new Date();
    cust.save();
    return cust;
  }

  /**
   * Get features for this customer.
   */
  public JsonNode getFeatures() {
    return features == null ? Json.newObject() : features;
  }

  /**
   * Upserts features for this customer. If updating a feature, only specified features will
   * be updated.
   */
  public void upsertFeatures(JsonNode input) {
    if (!input.isObject()) {
      throw new RuntimeException("Features must be Jsons.");
    } else if (features == null || features.isNull() || features.size() == 0) {
      features = input;
    } else {
      deepMerge(features, input);
    }
    save();
  }
}
