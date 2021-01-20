// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.common;

import com.yugabyte.yw.forms.CertificateParams;
import com.yugabyte.yw.models.CertificateInfo;
import org.bouncycastle.asn1.x500.X500Name;
import org.bouncycastle.asn1.x500.X500NameBuilder;
import org.bouncycastle.asn1.x500.style.BCStyle;
import org.bouncycastle.asn1.x509.BasicConstraints;
import org.bouncycastle.asn1.x509.Extension;
import org.bouncycastle.asn1.x509.KeyUsage;
import org.bouncycastle.cert.X509CertificateHolder;
import org.bouncycastle.cert.X509v3CertificateBuilder;
import org.bouncycastle.cert.jcajce.JcaX509CertificateConverter;
import org.bouncycastle.cert.jcajce.JcaX509CertificateHolder;
import org.bouncycastle.cert.jcajce.JcaX509ExtensionUtils;
import org.bouncycastle.cert.jcajce.JcaX509v3CertificateBuilder;
import org.bouncycastle.jce.provider.BouncyCastleProvider;
import org.bouncycastle.openssl.jcajce.JcaPEMWriter;
import org.bouncycastle.operator.ContentSigner;
import org.bouncycastle.operator.OperatorCreationException;
import org.bouncycastle.operator.jcajce.JcaContentSignerBuilder;
import org.bouncycastle.pkcs.PKCS10CertificationRequest;
import org.bouncycastle.pkcs.PKCS10CertificationRequestBuilder;
import org.bouncycastle.pkcs.jcajce.JcaPKCS10CertificationRequestBuilder;
import org.bouncycastle.util.io.pem.PemObject;
import org.bouncycastle.util.io.pem.PemReader;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import org.flywaydb.play.FileUtils;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import play.libs.Json;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.StringWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.math.BigInteger;
import java.security.InvalidKeyException;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.NoSuchAlgorithmException;
import java.security.NoSuchProviderException;
import java.security.PrivateKey;
import java.security.Security;
import java.security.SignatureException;
import java.security.cert.CertificateException;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.InvalidKeySpecException;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Base64;
import java.util.Calendar;
import java.util.Date;
import java.util.List;
import java.util.UUID;

/**
 * Helper class for Certificates
 */

public class CertificateHelper {

  public static final Logger LOG = LoggerFactory.getLogger(CertificateHelper.class);

  public static final String CLIENT_CERT = "yugabytedb.crt";
  public static final String CLIENT_KEY = "yugabytedb.key";
  public static final String DEFAULT_CLIENT = "yugabyte";
  public static final String CERT_PATH = "%s/certs/%s/%s";
  public static final String ROOT_CERT = "root.crt";

  public static UUID createRootCA(String nodePrefix, UUID customerUUID, String storagePath) {
      try {
        Security.addProvider(new BouncyCastleProvider());
        KeyPairGenerator keypairGen = KeyPairGenerator.getInstance("RSA");
        keypairGen.initialize(2048);
        UUID rootCA_UUID = UUID.randomUUID();
        KeyPair keyPair = keypairGen.generateKeyPair();
        Calendar cal = Calendar.getInstance();
        Date certStart = cal.getTime();
        cal.add(Calendar.YEAR, 1);
        Date certExpiry = cal.getTime();
        X500Name subject = new X500NameBuilder(BCStyle.INSTANCE)
          .addRDN(BCStyle.CN, nodePrefix)
          .addRDN(BCStyle.O, "example.com")
          .build();
        BigInteger serial = BigInteger.valueOf(System.currentTimeMillis());
        X509v3CertificateBuilder certGen = new JcaX509v3CertificateBuilder(
          subject,
          serial,
          certStart,
          certExpiry,
          subject,
          keyPair.getPublic());
        BasicConstraints basicConstraints = new BasicConstraints(1);
        KeyUsage keyUsage = new KeyUsage(KeyUsage.digitalSignature | KeyUsage.nonRepudiation |
                                         KeyUsage.keyEncipherment | KeyUsage.keyCertSign);
        certGen.addExtension(
          Extension.basicConstraints,
          true,
          basicConstraints.toASN1Primitive());
        certGen.addExtension(
          Extension.keyUsage,
          true,
          keyUsage.toASN1Primitive());
        ContentSigner signer = new JcaContentSignerBuilder("SHA256withRSA")
          .build(keyPair.getPrivate());
        X509CertificateHolder holder = certGen.build(signer);
        JcaX509CertificateConverter converter = new JcaX509CertificateConverter();
        converter.setProvider(new BouncyCastleProvider());
        X509Certificate x509 = converter.getCertificate(holder);
        String certPath = String.format(CERT_PATH + "/ca.%s", storagePath,
            customerUUID.toString(), rootCA_UUID.toString(), ROOT_CERT);
        String keyPath = String.format(CERT_PATH + "/ca.key.pem", storagePath,
            customerUUID.toString(), rootCA_UUID.toString());
        File certfile = new File(certPath);
        certfile.getParentFile().mkdirs();
        File keyfile = new File(keyPath);
        JcaPEMWriter certWriter = new JcaPEMWriter(new FileWriter(certfile));
        JcaPEMWriter keyWriter = new JcaPEMWriter(new FileWriter(keyfile));
        certWriter.writeObject(x509);
        certWriter.flush();
        keyWriter.writeObject(keyPair.getPrivate());
        keyWriter.flush();
        CertificateInfo.Type certType = CertificateInfo.Type.SelfSigned;
        LOG.info(
          "Generated self signed cert label {} uuid {} of type {} for customer {} at paths {}, {}",
          nodePrefix, rootCA_UUID, certType, customerUUID,
          certPath, keyPath
        );

        CertificateInfo cert = CertificateInfo.create(
          rootCA_UUID, customerUUID, nodePrefix,
          certStart, certExpiry, keyPath, certPath, certType
        );

        LOG.info("Created Root CA for {}.", nodePrefix);
        return cert.uuid;
      } catch (NoSuchAlgorithmException | IOException | OperatorCreationException |
               CertificateException e) {
        LOG.error("Unable to create RootCA for universe " + nodePrefix, e);
        return null;
      }
  }

  public static JsonNode createClientCertificate(UUID rootCA, String storagePath, String username,
                                                 Date certStart, Date certExpiry) {
    LOG.info("Creating client certificate signed by root CA {} and user {} at path {}",
            rootCA, username, storagePath);
    try {
      // Add the security provider in case createRootCA was never called.
      Security.addProvider(new BouncyCastleProvider());
      KeyPairGenerator keypairGen = KeyPairGenerator.getInstance("RSA");
      keypairGen.initialize(2048);

      Calendar cal = Calendar.getInstance();
      if (certStart == null) {
        certStart = cal.getTime();
      }
      if (certExpiry == null) {
        cal.add(Calendar.YEAR, 1);
        certExpiry = cal.getTime();
      }

      CertificateInfo cert = CertificateInfo.get(rootCA);
      if (cert.privateKey == null) {
        throw new RuntimeException("Keyfile cannot be null!");
      }

      FileInputStream is = new FileInputStream(new File(cert.certificate));
      CertificateFactory fact = CertificateFactory.getInstance("X.509");
      X509Certificate cer = (X509Certificate) fact.generateCertificate(is);
      X500Name subject = new JcaX509CertificateHolder(cer).getSubject();
      PemReader pemReader = new PemReader(new FileReader(cert.privateKey));
      PemObject pemObject = pemReader.readPemObject();
      byte[] bytes = pemObject.getContent();
      pemReader.close();
      PKCS8EncodedKeySpec spec = new PKCS8EncodedKeySpec(bytes);
      KeyFactory kf = null;
      PrivateKey pk = null;
      try {
        kf = KeyFactory.getInstance("RSA");
        pk = kf.generatePrivate(spec);
      } catch (InvalidKeySpecException e) {
        LOG.error("Unable to create client CA for username {} using root CA {}",
                  username, rootCA, e);
        throw new RuntimeException("Could not create client cert.");
      }

      X500Name clientCertSubject = new X500Name(String.format("CN=%s",username));
      BigInteger clientSerial = BigInteger.valueOf(System.currentTimeMillis());
      KeyPair clientKeyPair = keypairGen.generateKeyPair();
      PKCS10CertificationRequestBuilder p10Builder = new JcaPKCS10CertificationRequestBuilder(
        clientCertSubject,
        clientKeyPair.getPublic());
      ContentSigner csrContentSigner = new JcaContentSignerBuilder("SHA256withRSA")
          .build(pk);
      PKCS10CertificationRequest csr = p10Builder.build(csrContentSigner);

      KeyUsage keyUsage = new KeyUsage(KeyUsage.digitalSignature | KeyUsage.nonRepudiation |
                                           KeyUsage.keyEncipherment | KeyUsage.keyCertSign);

      X509v3CertificateBuilder clientCertBuilder = new X509v3CertificateBuilder(
        subject, clientSerial, certStart, certExpiry,
        csr.getSubject(), csr.getSubjectPublicKeyInfo());
      JcaX509ExtensionUtils clientCertExtUtils = new JcaX509ExtensionUtils();
      clientCertBuilder.addExtension(Extension.basicConstraints, true,
                                     new BasicConstraints(false).toASN1Primitive());
      clientCertBuilder.addExtension(Extension.authorityKeyIdentifier, false,
                                     clientCertExtUtils.createAuthorityKeyIdentifier(cer));
      clientCertBuilder.addExtension(Extension.subjectKeyIdentifier, false,
          clientCertExtUtils.createSubjectKeyIdentifier(csr.getSubjectPublicKeyInfo()));
      clientCertBuilder.addExtension(Extension.keyUsage, false, keyUsage.toASN1Primitive());

      X509CertificateHolder clientCertHolder = clientCertBuilder.build(csrContentSigner);
      X509Certificate clientCert  = new JcaX509CertificateConverter()
        .setProvider(new BouncyCastleProvider())
        .getCertificate(clientCertHolder);

      clientCert.verify(cer.getPublicKey(), "BC");

      JcaPEMWriter clientCertWriter;
      JcaPEMWriter clientKeyWriter;
      StringWriter certWriter = new StringWriter();
      StringWriter keyWriter = new StringWriter();
      ObjectNode bodyJson = Json.newObject();
      if (storagePath != null) {
        String clientCertPath = String.format("%s/%s", storagePath, CLIENT_CERT);
        String clientKeyPath = String.format("%s/%s", storagePath, CLIENT_KEY);
        File clientCertfile = new File(clientCertPath);
        File clientKeyfile = new File(clientKeyPath);
        clientCertWriter = new JcaPEMWriter(new FileWriter(clientCertfile));
        clientKeyWriter = new JcaPEMWriter(new FileWriter(clientKeyfile));
      } else {
        clientCertWriter = new JcaPEMWriter(certWriter);
        clientKeyWriter = new JcaPEMWriter(keyWriter);
      }
      clientCertWriter.writeObject(clientCert);
      clientCertWriter.flush();
      clientKeyWriter.writeObject(clientKeyPair.getPrivate());
      clientKeyWriter.flush();
      if (storagePath == null) {
        bodyJson.put(CLIENT_CERT, certWriter.toString());
        bodyJson.put(CLIENT_KEY, keyWriter.toString());
      }
      LOG.info("Created Client CA for username {} signed by root CA {}.", username, rootCA);
      return bodyJson;

    } catch (NoSuchAlgorithmException | IOException | OperatorCreationException |
             CertificateException | InvalidKeyException | NoSuchProviderException |
             SignatureException e) {
      LOG.error("Unable to create client CA for username {} using root CA {}", username, rootCA, e);
      throw new RuntimeException("Could not create client cert.");
    }
  }

  public static UUID uploadRootCA(
    String label, UUID customerUUID, String storagePath,
    String certContent, String keyContent, Date certStart,
    Date certExpiry, CertificateInfo.Type certType,
    CertificateParams.CustomCertInfo customCertInfo) throws IOException {

    if (certContent == null) {
      throw new RuntimeException("Certfile can't be null");
    }
    UUID rootCA_UUID = UUID.randomUUID();
    String keyPath = null;
    if (certType == CertificateInfo.Type.SelfSigned) {
      keyPath = String.format("%s/certs/%s/%s/ca.key.pem", storagePath,
                                   customerUUID.toString(), rootCA_UUID.toString());
    }
    String certPath = String.format("%s/certs/%s/%s/ca.%s", storagePath,
                                    customerUUID.toString(), rootCA_UUID.toString(), ROOT_CERT);

    File certfile = new File(certPath);
    // Create directory to store the keys.
    certfile.getParentFile().mkdirs();
    Files.write(certfile.toPath(), certContent.getBytes());
    if (certType == CertificateInfo.Type.SelfSigned) {
      File keyfile = new File(keyPath);
      Files.write(keyfile.toPath(), keyContent.getBytes());
    }
    LOG.info(
      "Uploaded cert label {} (uuid {}) of type {} at paths {}, {}",
      label, rootCA_UUID, certType,
      certPath, ((keyPath == null) ? "no private key" : keyPath)
    );
    CertificateInfo cert;
    try {
      if (certType == CertificateInfo.Type.SelfSigned) {
        cert = CertificateInfo.create(rootCA_UUID, customerUUID, label, certStart,
          certExpiry, keyPath, certPath, certType);
      } else {
        cert = CertificateInfo.create(rootCA_UUID, customerUUID, label, certStart,
          certExpiry, certPath, customCertInfo);
      }
      return cert.uuid;
    } catch (IOException | NoSuchAlgorithmException e) {
      LOG.error("Could not generate checksum for cert");
      throw new RuntimeException("Checksum generation failed.");
    }
  }

  public static String getCertPEMFileContents(UUID rootCA) {
    CertificateInfo cert = CertificateInfo.get(rootCA);
    String certPEM = FileUtils.readFileToString(new File(cert.certificate));
    return certPEM;
  }

  public static String getCertPEM(UUID rootCA){
    String certPEM = getCertPEMFileContents(rootCA);
    certPEM = Base64.getEncoder().encodeToString(certPEM.getBytes());
    return certPEM;
  }

  public static String getCertPEM(CertificateInfo cert){
    String certPEM = FileUtils.readFileToString(new File(cert.certificate));
    certPEM = Base64.getEncoder().encodeToString(certPEM.getBytes());
    return certPEM;
  }

  public static String getKeyPEM(CertificateInfo cert){
    String privateKeyPEM = FileUtils.readFileToString(new File(cert.privateKey));
    privateKeyPEM = Base64.getEncoder().encodeToString(privateKeyPEM.getBytes());
    return privateKeyPEM;
  }

  public static String getKeyPEM(UUID rootCA){
    CertificateInfo cert = CertificateInfo.get(rootCA);
    String privateKeyPEM = FileUtils.readFileToString(new File(cert.privateKey));
    privateKeyPEM = Base64.getEncoder().encodeToString(privateKeyPEM.getBytes());
    return privateKeyPEM;
  }

  public static String getClientCertFile(UUID rootCA) {
    CertificateInfo cert = CertificateInfo.get(rootCA);
    File certFile = new File(cert.certificate);
    String path = certFile.getParentFile().toString();
    return String.format("%s/%s", path, CLIENT_CERT);
  }

  public static String getClientKeyFile(UUID rootCA) {
    CertificateInfo cert = CertificateInfo.get(rootCA);
    File certFile = new File(cert.certificate);
    String path = certFile.getParentFile().toString();
    return String.format("%s/%s", path, CLIENT_KEY);
  }

  public static boolean areCertsDiff(UUID cert1, UUID cert2) {
    try {
      CertificateInfo cer1 = CertificateInfo.get(cert1);
      CertificateInfo cer2 = CertificateInfo.get(cert2);
      FileInputStream is1 = new FileInputStream(new File(cer1.certificate));
      FileInputStream is2 = new FileInputStream(new File(cer2.certificate));
      CertificateFactory fact = CertificateFactory.getInstance("X.509");
      X509Certificate certObj1 = (X509Certificate) fact.generateCertificate(is1);
      X509Certificate certObj2 = (X509Certificate) fact.generateCertificate(is2);
      return !certObj2.equals(certObj1);
    } catch (IOException | CertificateException e) {
      LOG.error("Unable to read certs {}: {}", cert1.toString(), cert2.toString());
      throw new RuntimeException("Could not read certs to compare. " + e);
    }
  }

  public static boolean arePathsSame(UUID cert1, UUID cert2) {
    CertificateInfo cer1 = CertificateInfo.get(cert1);
    CertificateInfo cer2 = CertificateInfo.get(cert2);
    return (cer1.getCustomCertInfo().nodeCertPath.equals(cer2.getCustomCertInfo().nodeCertPath) ||
            cer1.getCustomCertInfo().nodeKeyPath.equals(cer2.getCustomCertInfo().nodeKeyPath));
  }

  public static void createChecksums() {
    List<CertificateInfo> certs = CertificateInfo.getAllNoChecksum();
    for (CertificateInfo cert : certs) {
      try {
        cert.setChecksum();
      } catch (IOException | NoSuchAlgorithmException e) {
        // Log error, but don't cause it to error out.
        LOG.error("Could not generate checksum for cert: {}", cert.certificate);
      }
    }
  }
}
