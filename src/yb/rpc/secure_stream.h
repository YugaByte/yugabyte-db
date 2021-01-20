// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#ifndef YB_RPC_SECURE_STREAM_H
#define YB_RPC_SECURE_STREAM_H

#include "yb/rpc/stream.h"

#include "yb/rpc/circular_read_buffer.h"

#include "yb/util/enums.h"

typedef struct bio_st BIO;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct x509_st X509;
typedef struct x509_store_ctx_st X509_STORE_CTX;

namespace yb {
namespace rpc {

YB_DEFINE_ENUM(SecureState, (kInitial)(kHandshake)(kEnabled)(kDisabled));

#define YB_RPC_SSL_TYPE_DECLARE(name) \
  struct BOOST_PP_CAT(name, Free) { \
    void operator()(name* value) const; \
  }; \
  \
  typedef std::unique_ptr<name, BOOST_PP_CAT(name, Free)> BOOST_PP_CAT(name, Ptr);


namespace detail {

YB_RPC_SSL_TYPE_DECLARE(BIO);
YB_RPC_SSL_TYPE_DECLARE(EVP_PKEY);
YB_RPC_SSL_TYPE_DECLARE(SSL);
YB_RPC_SSL_TYPE_DECLARE(SSL_CTX);
YB_RPC_SSL_TYPE_DECLARE(X509);

} // namespace detail

class SecureContext {
 public:
  SecureContext();

  SecureContext(const SecureContext&) = delete;
  void operator=(const SecureContext&) = delete;

  CHECKED_STATUS AddCertificateAuthority(const Slice& data);
  CHECKED_STATUS AddCertificateAuthorityFile(const std::string& file);

  CHECKED_STATUS UsePrivateKey(const Slice& data);
  CHECKED_STATUS UseCertificate(const Slice& data);

  // Generates and uses temporary keys, should be used only during testing.
  CHECKED_STATUS TEST_GenerateKeys(int bits, const std::string& common_name);

  detail::SSLPtr Create() const;
  EVP_PKEY* private_key() const { return pkey_.get(); }
  X509* certificate() const { return certificate_.get(); }

  void set_require_client_certificate(bool value) {
    require_client_certificate_ = value;
  }

  bool require_client_certificate() const {
    return require_client_certificate_;
  }

  void set_use_client_certificate(bool value) {
    use_client_certificate_ = value;
  }

  bool use_client_certificate() const {
    return use_client_certificate_;
  }

 private:
  CHECKED_STATUS AddCertificateAuthority(X509* cert);

  detail::SSL_CTXPtr context_;
  detail::EVP_PKEYPtr pkey_;
  detail::X509Ptr certificate_;
  bool require_client_certificate_ = false;
  bool use_client_certificate_ = false;
};

const Protocol* SecureStreamProtocol();
StreamFactoryPtr SecureStreamFactory(
    StreamFactoryPtr lower_layer_factory, const MemTrackerPtr& buffer_tracker,
    SecureContext* context);

} // namespace rpc
} // namespace yb

#endif // YB_RPC_SECURE_STREAM_H
