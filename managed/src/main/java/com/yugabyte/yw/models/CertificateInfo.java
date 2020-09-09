// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import io.ebean.*;
import com.fasterxml.jackson.annotation.JsonFormat;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.validation.Constraints;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import java.util.Date;
import java.util.List;
import java.util.UUID;

@Entity
public class CertificateInfo extends Model {

    @Constraints.Required
    @Id
    @Column(nullable = false, unique = true)
    public UUID uuid;

    @Constraints.Required
    @Column(nullable = false)
    public UUID customerUUID;

    @Column(unique = true)
    public String label;

    @Constraints.Required
    @Column(nullable = false)
    @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
    public Date startDate;

    @Constraints.Required
    @Column(nullable = false)
    @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
    public Date expiryDate;

    @Constraints.Required
    @Column(nullable = false)
    public String privateKey;

    @Constraints.Required
    @Column(nullable = false)
    public String certificate;

    public static final Logger LOG = LoggerFactory.getLogger(CertificateInfo.class);

    public static CertificateInfo create(UUID uuid, UUID customerUUID, String label, Date startDate, Date expiryDate, String privateKey, String certificate) {
        CertificateInfo cert = new CertificateInfo();
        cert.uuid = uuid;
        cert.customerUUID = customerUUID;
        cert.label = label;
        cert.startDate = startDate;
        cert.expiryDate = expiryDate;
        cert.privateKey = privateKey;
        cert.certificate = certificate;
        cert.save();
        return cert;
    }

    private static final Finder<UUID, CertificateInfo> find =
      new Finder<UUID, CertificateInfo>(CertificateInfo.class) {};

    public static CertificateInfo get(UUID certUUID) {
        return find.byId(certUUID);
    }

    public static CertificateInfo get(String label) {
        return find.query().where().eq("label", label).findOne();
    }


    public static List<CertificateInfo> getAll(UUID customerUUID) {
        return find.query().where().eq("customer_uuid", customerUUID).findList();
    }
}
