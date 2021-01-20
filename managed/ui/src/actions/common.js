// Copyright (c) YugaByte, Inc.
import { ROOT_URL } from '../config';

export function getProviderEndpoint(providerUUID) {
  const customerUUID = localStorage.getItem('customerId');
  return `${ROOT_URL}/customers/${customerUUID}/providers/${providerUUID}`;
}

export function getCustomerEndpoint() {
  const customerUUID = localStorage.getItem('customerId');
  return `${ROOT_URL}/customers/${customerUUID}`;
}

export function getUniverseEndpoint(universeUUID) {
  const baseUrl = getCustomerEndpoint();
  return `${baseUrl}/universes/${universeUUID}`;
}

export function getTablesEndpoint(universeUUID, tableUUID) {
  const baseUrl = getUniverseEndpoint(universeUUID);
  return `${baseUrl}/tables/${tableUUID}`;
}
