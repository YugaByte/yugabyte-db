/*
 * Copyright 2021 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * http://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common.password;

import com.typesafe.config.Config;
import com.yugabyte.yw.common.YWServiceException;
import com.yugabyte.yw.forms.PasswordPolicyFormData;
import com.yugabyte.yw.models.CustomerConfig;
import org.apache.commons.lang3.ArrayUtils;
import org.apache.commons.lang3.StringUtils;
import play.data.validation.ValidationError;
import play.libs.Json;

import javax.inject.Inject;
import javax.inject.Singleton;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.UUID;
import java.util.stream.Collectors;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.core.JsonProcessingException;

import static play.mvc.Http.Status.BAD_REQUEST;

@Singleton
public class PasswordPolicyService {
  private static final char[] SPECIAL_CHARACTERS =
      "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~".toCharArray();
  private static final String DEFAULT_MIN_LENGTH_PARAM = "yb.pwdpolicy.default_min_length";
  private static final String DEFAULT_MIN_UPPERCASE_PARAM = "yb.pwdpolicy.default_min_uppercase";
  private static final String DEFAULT_MIN_LOWERCASE_PARAM = "yb.pwdpolicy.default_min_lowercase";
  private static final String DEFAULT_MIN_DIGITS_PARAM = "yb.pwdpolicy.default_min_digits";
  private static final String DEFAULT_MIN_SPECIAL_CHAR_PARAM =
      "yb.pwdpolicy.default_min_special_chars";
  private List<PasswordValidator> validators = new ArrayList<>();

  private final Config config;

  @Inject
  public PasswordPolicyService(Config config) {
    this.config = config;
    validators.add(
        new PasswordComplexityValidator(
            PasswordPolicyFormData::getMinLength, c -> true, "characters"));
    validators.add(
        new PasswordComplexityValidator(
            PasswordPolicyFormData::getMinUppercase, Character::isUpperCase, "upper case letters"));
    validators.add(
        new PasswordComplexityValidator(
            PasswordPolicyFormData::getMinLowercase, Character::isLowerCase, "lower case letters"));
    validators.add(
        new PasswordComplexityValidator(
            PasswordPolicyFormData::getMinDigits, Character::isDigit, "digits"));
    validators.add(
        new PasswordComplexityValidator(
            PasswordPolicyFormData::getMinSpecialCharacters,
            c -> ArrayUtils.contains(SPECIAL_CHARACTERS, c),
            "special characters"));
  }

  public void checkPasswordPolicy(UUID customerUUID, String password) {
    PasswordPolicyFormData configuredPolicy =
        PasswordPolicyFormData.fromCustomerConfig(
            CustomerConfig.getPasswordPolicyConfig(customerUUID));

    PasswordPolicyFormData effectivePolicy;
    if (configuredPolicy == null) {
      effectivePolicy = new PasswordPolicyFormData();
      effectivePolicy.setMinLength(config.getInt(DEFAULT_MIN_LENGTH_PARAM));
      effectivePolicy.setMinUppercase(config.getInt(DEFAULT_MIN_UPPERCASE_PARAM));
      effectivePolicy.setMinLowercase(config.getInt(DEFAULT_MIN_LOWERCASE_PARAM));
      effectivePolicy.setMinDigits(config.getInt(DEFAULT_MIN_DIGITS_PARAM));
      effectivePolicy.setMinSpecialCharacters(config.getInt(DEFAULT_MIN_SPECIAL_CHAR_PARAM));
    } else {
      effectivePolicy = configuredPolicy;
    }

    if (StringUtils.isEmpty(password)) {
      throw new YWServiceException(BAD_REQUEST, "Password shouldn't be empty.");
    }

    List<ValidationError> errors =
        validators
            .stream()
            .map(validator -> validator.validate(password, effectivePolicy))
            .filter(Objects::nonNull)
            .collect(Collectors.toList());

    if (!errors.isEmpty()) {
      String fullMessage =
          errors
              .stream()
              .map(ValidationError::messages)
              .flatMap(List::stream)
              .collect(Collectors.joining("; "));

      throw new YWServiceException(BAD_REQUEST, fullMessage);
    }
  }

  // Method to return the password policy
  public PasswordPolicyFormData getPasswordPolicyData() {
    PasswordPolicyFormData effectivePolicy;
    PasswordPolicyFormData policyData;
    effectivePolicy = new PasswordPolicyFormData();
    effectivePolicy.setMinLength(config.getInt(DEFAULT_MIN_LENGTH_PARAM));
    effectivePolicy.setMinUppercase(config.getInt(DEFAULT_MIN_UPPERCASE_PARAM));
    effectivePolicy.setMinLowercase(config.getInt(DEFAULT_MIN_LOWERCASE_PARAM));
    effectivePolicy.setMinDigits(config.getInt(DEFAULT_MIN_DIGITS_PARAM));
    effectivePolicy.setMinSpecialCharacters(config.getInt(DEFAULT_MIN_SPECIAL_CHAR_PARAM));

    JsonNode policyJson = Json.toJson(effectivePolicy);
    ObjectMapper mapper = new ObjectMapper();
    
     try {
      policyData = mapper.treeToValue(policyJson, PasswordPolicyFormData.class);
    } catch (JsonProcessingException e) {
      throw new RuntimeException("Can not pretty print a Json object.");
    }

    return policyData;
  }
}
