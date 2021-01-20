import axios, { Canceler } from 'axios';
import { ROOT_URL } from '../../config';
import {
  AvailabilityZone,
  Provider,
  Region,
  Universe,
  UniverseDetails,
  InstanceType,
  AccessKey,
  Certificate,
  KmsConfig,
  UniverseConfigure
} from './dtos';

// define unique names to use them as query keys
export enum QUERY_KEY {
  fetchUniverse = 'fetchUniverse',
  getProvidersList = 'getProvidersList',
  getRegionsList = 'getRegionsList',
  universeConfigure = 'universeConfigure',
  getInstanceTypes = 'getInstanceTypes',
  getDBVersions = 'getDBVersions',
  getAccessKeys = 'getAccessKeys',
  getCertificates = 'getCertificates',
  getKMSConfigs = 'getKMSConfigs'
}

class ApiService {
  private cancellers: Record<string, Canceler> = {};

  private getCustomerId(): string {
    const customerId = localStorage.getItem('customerId');
    return customerId || '';
  }

  findUniverseByName = (universeName: string): Promise<Universe> => {
    // auto-cancel previous request, if any
    if (this.cancellers.findUniverseByName) this.cancellers.findUniverseByName();

    // update cancellation stuff
    const source = axios.CancelToken.source();
    this.cancellers.findUniverseByName = source.cancel;

    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/universes/find/${universeName}`;
    return axios
      .get<Universe>(requestUrl, { cancelToken: source.token })
      .then((resp) => resp.data);
  };

  fetchUniverse = (queryKey: QUERY_KEY, universeId: string): Promise<Universe> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/universes/${universeId}`;
    return axios.get<Universe>(requestUrl).then((resp) => resp.data);
  };

  getProvidersList = (): Promise<Provider[]> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/providers`;
    return axios.get<Provider[]>(requestUrl).then((resp) => resp.data);
  };

  getRegionsList = (queryKey: QUERY_KEY, providerId: string): Promise<Region[]> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/providers/${providerId}/regions`;
    return axios.get<Region[]>(requestUrl).then((resp) => resp.data);
  };

  getAZList = (providerId: string, regionId: string): Promise<AvailabilityZone[]> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/providers/${providerId}/regions/${regionId}/zones`;
    return axios.get<AvailabilityZone[]>(requestUrl).then((resp) => resp.data);
  };

  universeConfigure = (queryKey: QUERY_KEY, data: UniverseConfigure): Promise<UniverseDetails> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/universe_configure`;
    return axios.post<UniverseDetails>(requestUrl, data).then((resp) => resp.data);
  };

  universeCreate = (data: UniverseConfigure): Promise<Universe> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/universes`;
    return axios.post<Universe>(requestUrl, data).then((resp) => resp.data);
  };

  universeEdit = (data: UniverseConfigure, universeId: string): Promise<Universe> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/universes/${universeId}`;
    return axios.put<Universe>(requestUrl, data).then((resp) => resp.data);
  };

  getInstanceTypes = (queryKey: QUERY_KEY, providerId: string): Promise<InstanceType[]> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/providers/${providerId}/instance_types`;
    return axios.get<InstanceType[]>(requestUrl).then((resp) => resp.data);
  };

  getDBVersions = (): Promise<string[]> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/releases`;
    return axios.get<string[]>(requestUrl).then((resp) => resp.data);
  };

  getAccessKeys = (queryKey: QUERY_KEY, providerId: string): Promise<AccessKey[]> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/providers/${providerId}/access_keys`;
    return axios.get<AccessKey[]>(requestUrl).then((resp) => resp.data);
  };

  getCertificates = (): Promise<Certificate[]> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/certificates`;
    return axios.get<Certificate[]>(requestUrl).then((resp) => resp.data);
  };

  getKMSConfigs = (): Promise<KmsConfig[]> => {
    const requestUrl = `${ROOT_URL}/customers/${this.getCustomerId()}/kms_configs`;
    return axios.get<KmsConfig[]>(requestUrl).then((resp) => resp.data);
  };

  // check if exception was caused by canceling previous request
  isRequestCancelError(error: unknown): boolean {
    return axios.isCancel(error);
  }
}

export const api = new ApiService();
