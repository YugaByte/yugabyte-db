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

#include "yb/rpc/secure_stream.h"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <boost/tokenizer.hpp>

#include "yb/rpc/outbound_call.h"
#include "yb/rpc/outbound_data.h"
#include "yb/rpc/rpc_util.h"

#include "yb/util/enums.h"
#include "yb/util/errno.h"
#include "yb/util/memory/memory.h"
#include "yb/util/logging.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"
#include "yb/util/encryption_util.h"

using namespace std::literals;

DEFINE_bool(allow_insecure_connections, true, "Whether we should allow insecure connections.");
DEFINE_bool(dump_certificate_entries, false, "Whether we should dump certificate entries.");
DEFINE_string(ssl_protocols, "",
              "List of allowed SSL protocols (ssl2, ssl3, tls10, tls11, tls12). "
                  "Empty to allow TLS only.");

namespace yb {
namespace rpc {

namespace {

const unsigned char kContextId[] = { 'Y', 'u', 'g', 'a', 'B', 'y', 't', 'e' };

std::string SSLErrorMessage(uint64_t error) {
  auto message = ERR_reason_error_string(error);
  return message ? message : "no error";
}

class SecureOutboundData : public OutboundData {
 public:
  SecureOutboundData(RefCntBuffer buffer, OutboundDataPtr lower_data)
      : buffer_(std::move(buffer)), lower_data_(std::move(lower_data)) {}

  void Transferred(const Status& status, Connection* conn) override {
    if (lower_data_) {
      lower_data_->Transferred(status, conn);
    }
  }

  bool DumpPB(const DumpRunningRpcsRequestPB& req, RpcCallInProgressPB* resp) override {
    return false;
  }

  void Serialize(boost::container::small_vector_base<RefCntBuffer>* output) override {
    output->push_back(std::move(buffer_));
  }

  std::string ToString() const override {
    return Format("Secure[$0]", lower_data_);
  }

  size_t ObjectSize() const override { return sizeof(*this); }

  size_t DynamicMemoryUsage() const override { return DynamicMemoryUsageOf(buffer_, lower_data_); }

 private:
  RefCntBuffer buffer_;
  OutboundDataPtr lower_data_;
};

#define YB_RPC_SSL_TYPE_DEFINE(name) \
  void BOOST_PP_CAT(name, Free)::operator()(name* value) const { \
    BOOST_PP_CAT(name, _free)(value); \
  } \

#define YB_RPC_SSL_TYPE(name) YB_RPC_SSL_TYPE_DECLARE(name) YB_RPC_SSL_TYPE_DEFINE(name)

#define SSL_STATUS(type, format) STATUS_FORMAT(type, format, SSLErrorMessage(ERR_get_error()))

Result<detail::BIOPtr> BIOFromSlice(const Slice& data) {
  detail::BIOPtr bio(BIO_new_mem_buf(data.data(), data.size()));
  if (!bio) {
    return SSL_STATUS(IOError, "Create BIO failed: $0");
  }
  return std::move(bio);
}

Result<detail::X509Ptr> X509FromSlice(const Slice& data) {
  ERR_clear_error();

  auto bio = VERIFY_RESULT(BIOFromSlice(data));

  detail::X509Ptr cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
  if (!cert) {
    return SSL_STATUS(IOError, "Read cert failed: $0");
  }

  return std::move(cert);
}

YB_RPC_SSL_TYPE(ASN1_INTEGER);
YB_RPC_SSL_TYPE(RSA);
YB_RPC_SSL_TYPE(X509_NAME);

Result<detail::EVP_PKEYPtr> GeneratePrivateKey(int bits) {
  RSAPtr rsa(RSA_generate_key(bits, 65537, nullptr, nullptr));
  if (!rsa) {
    return SSL_STATUS(InvalidArgument, "Failed to generate private key: $0");
  }

  detail::EVP_PKEYPtr pkey(EVP_PKEY_new());
  auto res = EVP_PKEY_assign_RSA(pkey.get(), rsa.release());
  if (res != 1) {
    return SSL_STATUS(InvalidArgument, "Failed to assign private key: $0");
  }

  return std::move(pkey);
}

Result<detail::X509Ptr> CreateCertificate(
    EVP_PKEY* key, const std::string& common_name, EVP_PKEY* ca_pkey, X509* ca_cert) {
  detail::X509Ptr cert(X509_new());
  if (!cert) {
    return SSL_STATUS(IOError, "Failed to create new certificate: $0");
  }

  if (X509_set_version(cert.get(), 2) != 1) {
    return SSL_STATUS(IOError, "Failed to set certificate version: $0");
  }

  ASN1_INTEGERPtr aserial(ASN1_INTEGER_new());
  ASN1_INTEGER_set(aserial.get(), 0);
  if (!X509_set_serialNumber(cert.get(), aserial.get())) {
    return SSL_STATUS(IOError, "Failed to set serial number: $0");
  }

  X509_NAMEPtr name(X509_NAME_new());
  auto bytes = pointer_cast<const unsigned char*>(common_name.c_str());
  if (!X509_NAME_add_entry_by_txt(
      name.get(), "CN", MBSTRING_ASC, bytes, common_name.length(), -1, 0)) {
    return SSL_STATUS(IOError, "Failed to create subject: $0");
  }

  if (X509_set_subject_name(cert.get(), name.get()) != 1) {
    return SSL_STATUS(IOError, "Failed to set subject: $0");
  }

  X509_NAME* issuer = name.get();
  if (ca_cert) {
    issuer = X509_get_subject_name(ca_cert);
    if (!issuer) {
      return SSL_STATUS(IOError, "Failed to get CA subject name: $0");
    }
  }

  if (X509_set_issuer_name(cert.get(), issuer) != 1) {
    return SSL_STATUS(IOError, "Failed to set issuer: $0");
  }

  if (X509_set_pubkey(cert.get(), key) != 1) {
    return SSL_STATUS(IOError, "Failed to set public key: $0");
  }

  if (!X509_gmtime_adj(X509_get_notBefore(cert.get()), 0)) {
    return SSL_STATUS(IOError, "Failed to set not before: $0");
  }

  const auto k1Year = 365 * 24h;
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(k1Year).count();
  if (!X509_gmtime_adj(X509_get_notAfter(cert.get()), seconds)) {
    return SSL_STATUS(IOError, "Failed to set not after: $0");
  }

  if (ca_cert) {
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, ca_cert, cert.get(), nullptr, nullptr, 0);
  }

  if (!X509_sign(cert.get(), ca_pkey, EVP_sha256())) {
    return SSL_STATUS(IOError, "Sign failed: $0");
  }

  return std::move(cert);
}

const std::unordered_map<std::string, int64_t>& SSLProtocolMap() {
  static const std::unordered_map<std::string, int64_t> result = {
      {"ssl2", SSL_OP_NO_SSLv2},
      {"ssl3", SSL_OP_NO_SSLv3},
      {"tls10", SSL_OP_NO_TLSv1},
      {"tls11", SSL_OP_NO_TLSv1_1},
      {"tls12", SSL_OP_NO_TLSv1_2},
  };
  return result;
}

int64_t ProtocolsOption() {
  constexpr int64_t kDefaultProtocols = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;

  const std::string& ssl_protocols = FLAGS_ssl_protocols;
  if (ssl_protocols.empty()) {
    return kDefaultProtocols;
  }

  const auto& protocol_map = SSLProtocolMap();
  int64_t result = SSL_OP_NO_SSL_MASK;
  boost::tokenizer<> tokenizer(ssl_protocols);
  for (const auto& protocol : tokenizer) {
    auto it = protocol_map.find(protocol);
    if (it == protocol_map.end()) {
      LOG(DFATAL) << "Unknown SSL protocol: " << protocol;
      return kDefaultProtocols;
    }
    result &= ~it->second;
  }

  return result;
}

} // namespace

namespace detail {

YB_RPC_SSL_TYPE_DEFINE(BIO)
YB_RPC_SSL_TYPE_DEFINE(EVP_PKEY)
YB_RPC_SSL_TYPE_DEFINE(SSL)
YB_RPC_SSL_TYPE_DEFINE(SSL_CTX)
YB_RPC_SSL_TYPE_DEFINE(X509)

}

SecureContext::SecureContext() {
  yb::enterprise::InitOpenSSL();

  context_.reset(SSL_CTX_new(SSLv23_method()));
  DCHECK(context_);

  int64_t protocols = ProtocolsOption();
  VLOG(1) << "Protocols option: " << protocols;
  SSL_CTX_set_options(context_.get(), protocols | SSL_OP_NO_COMPRESSION);
  auto res = SSL_CTX_set_session_id_context(context_.get(), kContextId, sizeof(kContextId));
  LOG_IF(DFATAL, res != 1) << "Failed to set session id for SSL context: "
                           << SSLErrorMessage(ERR_get_error());
}

detail::SSLPtr SecureContext::Create() const {
  return detail::SSLPtr(SSL_new(context_.get()));
}

Status SecureContext::AddCertificateAuthorityFile(const std::string& file) {
  X509_STORE* store = SSL_CTX_get_cert_store(context_.get());
  if (!store) {
    return SSL_STATUS(IllegalState, "Failed to get store: $0");
  }

  auto bytes = pointer_cast<const char*>(file.c_str());
  auto res = X509_STORE_load_locations(store, bytes, nullptr);
  if (res != 1) {
    return SSL_STATUS(InvalidArgument, "Failed to add certificate file: $0");
  }

  return Status::OK();
}

Status SecureContext::AddCertificateAuthority(const Slice& data) {
  return AddCertificateAuthority(VERIFY_RESULT(X509FromSlice(data)).get());
}

Status SecureContext::AddCertificateAuthority(X509* cert) {
  X509_STORE* store = SSL_CTX_get_cert_store(context_.get());
  if (!store) {
    return SSL_STATUS(IllegalState, "Failed to get store: $0");
  }

  auto res = X509_STORE_add_cert(store, cert);
  if (res != 1) {
    return SSL_STATUS(InvalidArgument, "Failed to add certificate: $0");
  }

  return Status::OK();
}

Status SecureContext::TEST_GenerateKeys(int bits, const std::string& common_name) {
  auto ca_key = VERIFY_RESULT(GeneratePrivateKey(bits));
  auto ca_cert = VERIFY_RESULT(CreateCertificate(ca_key.get(), "YugaByte", ca_key.get(), nullptr));
  auto key = VERIFY_RESULT(GeneratePrivateKey(bits));
  auto cert = VERIFY_RESULT(CreateCertificate(key.get(), common_name, ca_key.get(), ca_cert.get()));

  RETURN_NOT_OK(AddCertificateAuthority(ca_cert.get()));
  pkey_ = std::move(key);
  certificate_ = std::move(cert);

  return Status::OK();
}

Status SecureContext::UsePrivateKey(const Slice& slice) {
  ERR_clear_error();

  auto bio = VERIFY_RESULT(BIOFromSlice(slice));

  auto pkey = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
  if (!pkey) {
    return SSL_STATUS(IOError, "Failed to read private key: $0");
  }

  pkey_.reset(pkey);
  return Status::OK();
}

Status SecureContext::UseCertificate(const Slice& data) {
  ERR_clear_error();

  certificate_ = VERIFY_RESULT(X509FromSlice(data));

  return Status::OK();
}

namespace {

class SecureStream : public Stream, public StreamContext {
 public:
  SecureStream(const SecureContext& context, std::unique_ptr<Stream> lower_stream,
               size_t receive_buffer_size, const MemTrackerPtr& buffer_tracker,
               const StreamCreateData& data)
    : secure_context_(context), lower_stream_(std::move(lower_stream)),
      remote_hostname_(data.remote_hostname),
      encrypted_read_buffer_(receive_buffer_size, buffer_tracker) {
  }

  SecureStream(const SecureStream&) = delete;
  void operator=(const SecureStream&) = delete;

  size_t GetPendingWriteBytes() override {
    return lower_stream_->GetPendingWriteBytes();
  }

 private:
  CHECKED_STATUS Start(bool connect, ev::loop_ref* loop, StreamContext* context) override;
  void Close() override;
  void Shutdown(const Status& status) override;
  Result<size_t> Send(OutboundDataPtr data) override;
  CHECKED_STATUS TryWrite() override;
  void ParseReceived() override;

  void Cancelled(size_t handle) override {
    LOG_WITH_PREFIX(DFATAL) << "Cancel is not supported for secure stream: " << handle;
  }

  bool Idle(std::string* reason_not_idle) override;
  bool IsConnected() override;
  void DumpPB(const DumpRunningRpcsRequestPB& req, RpcConnectionPB* resp) override;

  const Endpoint& Remote() override;
  const Endpoint& Local() override;

  const Protocol* GetProtocol() override {
    return SecureStreamProtocol();
  }

  // Implementation StreamContext
  void UpdateLastActivity() override;
  void UpdateLastRead() override;
  void UpdateLastWrite() override;
  void Transferred(const OutboundDataPtr& data, const Status& status) override;
  void Destroy(const Status& status) override;
  Result<ProcessDataResult> ProcessReceived(
      const IoVecs& data, ReadBufferFull read_buffer_full) override;
  void Connected() override;

  StreamReadBuffer& ReadBuffer() override {
    return encrypted_read_buffer_;
  }

  CHECKED_STATUS Handshake();

  CHECKED_STATUS Init();
  CHECKED_STATUS Established(SecureState state);
  static int VerifyCallback(int preverified, X509_STORE_CTX* store_context);
  bool Verify(bool preverified, X509_STORE_CTX* store_context);
  CHECKED_STATUS SendEncrypted(OutboundDataPtr data);
  Result<bool> WriteEncrypted(OutboundDataPtr data);
  CHECKED_STATUS ReadDecrypted();
  Result<size_t> SslRead(void* buf, int num);

  std::string ToString() override;

  const SecureContext& secure_context_;
  std::unique_ptr<Stream> lower_stream_;
  const std::string remote_hostname_;
  StreamContext* context_;
  size_t decrypted_bytes_to_skip_ = 0;
  SecureState state_ = SecureState::kInitial;
  bool need_connect_ = false;
  bool connected_ = false;
  std::vector<OutboundDataPtr> pending_data_;
  std::vector<std::string> certificate_entries_;

  CircularReadBuffer encrypted_read_buffer_;

  detail::BIOPtr bio_;
  detail::SSLPtr ssl_;
};

Status SecureStream::Start(bool connect, ev::loop_ref* loop, StreamContext* context) {
  context_ = context;
  need_connect_ = connect;
  return lower_stream_->Start(connect, loop, this);
}

void SecureStream::Close() {
  lower_stream_->Close();
}

void SecureStream::Shutdown(const Status& status) {
  VLOG_WITH_PREFIX(1) << "SecureStream::Shutdown with status: " << status;

  for (auto& data : pending_data_) {
    if (data) {
      context_->Transferred(data, status);
    }
  }
  pending_data_.clear();

  lower_stream_->Shutdown(status);
}

Status SecureStream::SendEncrypted(OutboundDataPtr data) {
  boost::container::small_vector<RefCntBuffer, 10> queue;
  data->Serialize(&queue);
  for (const auto& buf : queue) {
    Slice slice(buf.data(), buf.size());
    for (;;) {
      auto len = SSL_write(ssl_.get(), slice.data(), slice.size());
      if (len == slice.size()) {
        break;
      }
      auto error = len <= 0 ? SSL_get_error(ssl_.get(), len) : SSL_ERROR_NONE;
      VLOG_WITH_PREFIX(4) << "SSL_write was not full: " << slice.size() << ", written: " << len
                          << ", error: " << error;
      if (error != SSL_ERROR_NONE) {
        if (error != SSL_ERROR_WANT_WRITE || !VERIFY_RESULT(WriteEncrypted(nullptr))) {
          return STATUS_FORMAT(
              NetworkError, "SSL write failed: $0 ($1)", SSLErrorMessage(error), error);
        }
      } else {
        RETURN_NOT_OK(WriteEncrypted(nullptr));
      }
      if (len > 0) {
        slice.remove_prefix(len);
      }
    }
  }
  return ResultToStatus(WriteEncrypted(std::move(data)));
}

Result<size_t> SecureStream::Send(OutboundDataPtr data) {
  switch (state_) {
  case SecureState::kInitial:
  case SecureState::kHandshake:
    pending_data_.push_back(std::move(data));
    return std::numeric_limits<size_t>::max();
  case SecureState::kEnabled:
    RETURN_NOT_OK(SendEncrypted(std::move(data)));
    return std::numeric_limits<size_t>::max();
  case SecureState::kDisabled:
    return lower_stream_->Send(std::move(data));
  }

  FATAL_INVALID_ENUM_VALUE(SecureState, state_);
}

Result<bool> SecureStream::WriteEncrypted(OutboundDataPtr data) {
  auto pending = BIO_ctrl_pending(bio_.get());
  if (pending == 0) {
    return data ? STATUS(NetworkError, "No pending data during write") : Result<bool>(false);
  }
  RefCntBuffer buf(pending);
  auto len = BIO_read(bio_.get(), buf.data(), buf.size());
  LOG_IF_WITH_PREFIX(DFATAL, len != buf.size())
      << "BIO_read was not full: " << buf.size() << ", read: " << len;
  VLOG_WITH_PREFIX(4) << "Write encrypted: " << len << ", " << yb::ToString(data);
  RETURN_NOT_OK(lower_stream_->Send(std::make_shared<SecureOutboundData>(buf, std::move(data))));
  return true;
}

Status SecureStream::TryWrite() {
  return lower_stream_->TryWrite();
}

void SecureStream::ParseReceived() {
  lower_stream_->ParseReceived();
}

bool SecureStream::Idle(std::string* reason) {
  return lower_stream_->Idle(reason);
}

bool SecureStream::IsConnected() {
  return connected_;
}

void SecureStream::DumpPB(const DumpRunningRpcsRequestPB& req, RpcConnectionPB* resp) {
  lower_stream_->DumpPB(req, resp);
}

const Endpoint& SecureStream::Remote() {
  return lower_stream_->Remote();
}

const Endpoint& SecureStream::Local() {
  return lower_stream_->Local();
}

std::string SecureStream::ToString() {
  return Format("SECURE[$0] $1 $2", need_connect_ ? "C" : "S", state_, lower_stream_->ToString());
}

void SecureStream::UpdateLastActivity() {
  context_->UpdateLastActivity();
}

void SecureStream::UpdateLastRead() {
  context_->UpdateLastRead();
}

void SecureStream::UpdateLastWrite() {
  context_->UpdateLastWrite();
}

void SecureStream::Transferred(const OutboundDataPtr& data, const Status& status) {
  context_->Transferred(data, status);
}

void SecureStream::Destroy(const Status& status) {
  context_->Destroy(status);
}

Result<ProcessDataResult> SecureStream::ProcessReceived(
    const IoVecs& data, ReadBufferFull read_buffer_full) {
  switch (state_) {
    case SecureState::kInitial: {
      if (data[0].iov_len < 2) {
        return ProcessDataResult{0, Slice()};
      }
      const uint8_t* bytes = static_cast<const uint8_t*>(data[0].iov_base);
      if (bytes[0] == 0x16 && bytes[1] == 0x03) { // TLS handshake header
        state_ = SecureState::kHandshake;
        ResetLogPrefix();
        RETURN_NOT_OK(Init());
      } else if (FLAGS_allow_insecure_connections) {
        RETURN_NOT_OK(Established(SecureState::kDisabled));
      } else {
        return STATUS_FORMAT(NetworkError, "Insecure connection header: $0",
                             Slice(bytes, 2).ToDebugHexString());
      }
      return ProcessReceived(data, read_buffer_full);
    }

    case SecureState::kDisabled:
      return context_->ProcessReceived(data, read_buffer_full);

    case SecureState::kHandshake: {
      size_t result = 0;
      for (const auto& iov : data) {
        auto written = BIO_write(bio_.get(), iov.iov_base, iov.iov_len);
        result += written;
        DCHECK_EQ(written, iov.iov_len);
      }
      auto handshake_status = Handshake();
      LOG_IF_WITH_PREFIX(INFO, !handshake_status.ok()) << "Handshake failed: " << handshake_status;
      RETURN_NOT_OK(handshake_status);
      return ProcessDataResult{ result, Slice() };
    }

    case SecureState::kEnabled: {
      size_t result = 0;
      for (const auto& iov : data) {
        Slice slice(static_cast<char*>(iov.iov_base), iov.iov_len);
        for (;;) {
          auto len = BIO_write(bio_.get(), slice.data(), slice.size());
          result += len;
          if (len == slice.size()) {
            break;
          }
          slice.remove_prefix(len);
          RETURN_NOT_OK(ReadDecrypted());
        }
      }
      RETURN_NOT_OK(ReadDecrypted());
      return ProcessDataResult{ result, Slice() };
    }
  }

  return STATUS_FORMAT(IllegalState, "Unexpected state: $0", to_underlying(state_));
}

// Tries to do SSL_read up to num bytes from buf. Possible results:
// > 0 - number of bytes actually read.
// = 0 - in case of SSL_ERROR_WANT_READ.
// Status with network error - in case of other errors.
Result<size_t> SecureStream::SslRead(void* buf, int num) {
  auto len = SSL_read(ssl_.get(), buf, num);
  if (len <= 0) {
    auto error = SSL_get_error(ssl_.get(), len);
    if (error == SSL_ERROR_WANT_READ) {
      return 0;
    } else {
      auto status = STATUS_FORMAT(
          NetworkError, "SSL read failed: $0 ($1)", SSLErrorMessage(error), error);
      LOG_WITH_PREFIX(INFO) << status;
      return status;
    }
  }
  return len;
}

Status SecureStream::ReadDecrypted() {
  // TODO handle IsBusy
  auto& decrypted_read_buffer = context_->ReadBuffer();
  bool done = false;
  while (!done) {
    if (decrypted_bytes_to_skip_ > 0) {
      auto global_skip_buffer = GetGlobalSkipBuffer();
      do {
        auto len = VERIFY_RESULT(SslRead(
            global_skip_buffer.mutable_data(),
            std::min(global_skip_buffer.size(), decrypted_bytes_to_skip_)));
        if (len == 0) {
          done = true;
          break;
        }
        VLOG_WITH_PREFIX(4) << "Skip decrypted: " << len;
        decrypted_bytes_to_skip_ -= len;
      } while (decrypted_bytes_to_skip_ > 0);
    }
    auto out = VERIFY_RESULT(decrypted_read_buffer.PrepareAppend());
    size_t appended = 0;
    for (auto iov = out.begin(); iov != out.end();) {
      auto len = VERIFY_RESULT(SslRead(iov->iov_base, iov->iov_len));
      if (len == 0) {
        done = true;
        break;
      }
      VLOG_WITH_PREFIX(4) << "Read decrypted: " << len;
      appended += len;
      iov->iov_base = static_cast<char*>(iov->iov_base) + len;
      iov->iov_len -= len;
      if (iov->iov_len <= 0) {
        ++iov;
      }
    }
    decrypted_read_buffer.DataAppended(appended);
    if (decrypted_read_buffer.ReadyToRead()) {
      auto temp = VERIFY_RESULT(context_->ProcessReceived(
          decrypted_read_buffer.AppendedVecs(), ReadBufferFull(decrypted_read_buffer.Full())));
      decrypted_read_buffer.Consume(temp.consumed, temp.buffer);
      DCHECK_EQ(decrypted_bytes_to_skip_, 0);
      decrypted_bytes_to_skip_ = temp.bytes_to_skip;
    }
  }

  return Status::OK();
}

void SecureStream::Connected() {
  if (need_connect_) {
    auto status = Init();
    if (status.ok()) {
      status = Handshake();
    }
    if (!status.ok()) {
      context_->Destroy(status);
    }
  }
}

Status SecureStream::Handshake() {
  for (;;) {
    if (state_ == SecureState::kEnabled) {
      return Status::OK();
    }

    auto pending_before = BIO_ctrl_pending(bio_.get());
    ERR_clear_error();
    int result = need_connect_ ? SSL_connect(ssl_.get()) : SSL_accept(ssl_.get());
    int ssl_error = SSL_get_error(ssl_.get(), result);
    int sys_error = static_cast<int>(ERR_get_error());
    auto pending_after = BIO_ctrl_pending(bio_.get());

    if (ssl_error == SSL_ERROR_SSL || ssl_error == SSL_ERROR_SYSCALL) {
      std::string message =
          ssl_error == SSL_ERROR_SSL ? SSLErrorMessage(sys_error) : ErrnoToString(sys_error);
      std::string certificate_entries;
      if (FLAGS_dump_certificate_entries) {
        certificate_entries = Format(", certificate entries: $0", certificate_entries_);
      }
      return STATUS_FORMAT(NetworkError, "Handshake failed: $0, address: $1, hostname: $2$3",
                           message, Remote().address(), remote_hostname_, certificate_entries);
    }

    if (ssl_error == SSL_ERROR_WANT_WRITE || pending_after > pending_before) {
      // SSL expects that we would write to underlying transport.
      RefCntBuffer buffer(pending_after);
      int len = BIO_read(bio_.get(), buffer.data(), buffer.size());
      DCHECK_EQ(len, pending_after);
      auto data = std::make_shared<SecureOutboundData>(buffer, nullptr);
      RETURN_NOT_OK(lower_stream_->Send(data));
      // If SSL_connect/SSL_accept returned positive result it means that TLS connection
      // was succesfully established. We just have to send last portion of data.
      if (result > 0) {
        RETURN_NOT_OK(Established(SecureState::kEnabled));
      }
    } else if (ssl_error == SSL_ERROR_WANT_READ) {
      // SSL expects that we would read from underlying transport.
      return Status::OK();
    } else if (SSL_get_shutdown(ssl_.get()) & SSL_RECEIVED_SHUTDOWN) {
      return STATUS(Aborted, "Handshake aborted");
    } else {
      return Established(SecureState::kEnabled);
    }
  }
}

Status SecureStream::Init() {
  if (!ssl_) {
    ssl_ = secure_context_.Create();
    SSL_set_mode(ssl_.get(), SSL_MODE_ENABLE_PARTIAL_WRITE);
    SSL_set_mode(ssl_.get(), SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_mode(ssl_.get(), SSL_MODE_RELEASE_BUFFERS);
    SSL_set_app_data(ssl_.get(), this);

    if (!need_connect_ || secure_context_.use_client_certificate()) {
      auto res = SSL_use_PrivateKey(ssl_.get(), secure_context_.private_key());
      if (res != 1) {
        return SSL_STATUS(InvalidArgument, "Failed to use private key: $0");
      }
      res = SSL_use_certificate(ssl_.get(), secure_context_.certificate());
      if (res != 1) {
        return SSL_STATUS(InvalidArgument, "Failed to use certificate: $0");
      }
    }

    BIO* int_bio = nullptr;
    BIO* temp_bio = nullptr;
    BIO_new_bio_pair(&int_bio, 0, &temp_bio, 0);
    SSL_set_bio(ssl_.get(), int_bio, int_bio);
    bio_.reset(temp_bio);

    int verify_mode = SSL_VERIFY_PEER;
    if (secure_context_.require_client_certificate()) {
      verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT | SSL_VERIFY_CLIENT_ONCE;
    }
    SSL_set_verify(ssl_.get(), verify_mode, &VerifyCallback);
  }

  return Status::OK();
}

Status SecureStream::Established(SecureState state) {
  VLOG_WITH_PREFIX(4) << "Established with state: " << state;

  state_ = state;
  ResetLogPrefix();
  connected_ = true;
  context_->Connected();
  for (auto& data : pending_data_) {
    RETURN_NOT_OK(Send(std::move(data)));
  }
  pending_data_.clear();
  return Status::OK();
}

int SecureStream::VerifyCallback(int preverified, X509_STORE_CTX* store_context) {
  if (store_context) {
    auto ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(
        store_context, SSL_get_ex_data_X509_STORE_CTX_idx()));
    if (ssl) {
      auto stream = static_cast<SecureStream*>(SSL_get_app_data(ssl));
      if (stream) {
        return stream->Verify(preverified != 0, store_context) ? 1 : 0;
      }
    }
  }

  return preverified;
}

namespace {

// Matches pattern from RFC 2818:
// Names may contain the wildcard character * which is considered to match any single domain name
// component or component fragment. E.g., *.a.com matches foo.a.com but not bar.foo.a.com.
// f*.com matches foo.com but not bar.com.
bool MatchPattern(Slice pattern, Slice host) {
  const char* p = pattern.cdata();
  const char* p_end = pattern.cend();
  const char* h = host.cdata();
  const char* h_end = host.cend();

  while (p != p_end && h != h_end) {
    if (*p == '*') {
      ++p;
      while (h != h_end && *h != '.') {
        if (MatchPattern(Slice(p, p_end), Slice(h, h_end))) {
          return true;
        }
        ++h;
      }
    } else if (std::tolower(*p) == std::tolower(*h)) {
      ++p;
      ++h;
    } else {
      return false;
    }
  }

  return p == p_end && h == h_end;
}

} // namespace

// Verify according to RFC 2818.
bool SecureStream::Verify(bool preverified, X509_STORE_CTX* store_context) {
  // Don't bother looking at certificates that have failed pre-verification.
  if (!preverified) {
    VLOG_WITH_PREFIX(4) << "Unverified certificate";
    return false;
  }

  // We're only interested in checking the certificate at the end of the chain.
  int depth = X509_STORE_CTX_get_error_depth(store_context);
  if (depth > 0) {
    VLOG_WITH_PREFIX(4) << "Intermediate certificate";
    return true;
  }

  X509* cert = X509_STORE_CTX_get_current_cert(store_context);

  auto gens = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, 0, 0));
  auto se = ScopeExit([gens] {
    GENERAL_NAMES_free(gens);
  });

  auto address = Remote().address();

  for (int i = 0; i < sk_GENERAL_NAME_num(gens); ++i) {
    GENERAL_NAME* gen = sk_GENERAL_NAME_value(gens, i);
    if (gen->type == GEN_DNS) {
      ASN1_IA5STRING* domain = gen->d.dNSName;
      if (domain->type == V_ASN1_IA5STRING && domain->data && domain->length) {
        Slice domain_slice(domain->data, domain->length);
        VLOG_WITH_PREFIX(4) << "Domain: " << domain_slice.ToBuffer() << " vs " << remote_hostname_;
        if (FLAGS_dump_certificate_entries) {
          certificate_entries_.push_back(Format("DNS:$0", domain_slice.ToBuffer()));
        }
        if (MatchPattern(domain_slice, remote_hostname_)) {
          return true;
        }
      }
    } else if (gen->type == GEN_IPADD) {
      ASN1_OCTET_STRING* ip_address = gen->d.iPAddress;
      if (ip_address->type == V_ASN1_OCTET_STRING && ip_address->data) {
        if (ip_address->length == 4) {
          boost::asio::ip::address_v4::bytes_type bytes;
          memcpy(&bytes, ip_address->data, bytes.size());
          auto allowed_address = boost::asio::ip::address_v4(bytes);
          VLOG_WITH_PREFIX(4) << "IPv4: " << allowed_address.to_string() << " vs " << address;
          if (FLAGS_dump_certificate_entries) {
            certificate_entries_.push_back(Format("IP Address:$0", allowed_address));
          }
          if (address == allowed_address) {
            return true;
          }
        } else if (ip_address->length == 16) {
          boost::asio::ip::address_v6::bytes_type bytes;
          memcpy(&bytes, ip_address->data, bytes.size());
          auto allowed_address = boost::asio::ip::address_v6(bytes);
          VLOG_WITH_PREFIX(4) << "IPv6: " << allowed_address.to_string() << " vs " << address;
          if (FLAGS_dump_certificate_entries) {
            certificate_entries_.push_back(Format("IP Address:$0", allowed_address));
          }
          if (address == allowed_address) {
            return true;
          }
        }
      }
    }
  }

  // No match in the alternate names, so try the common names. We should only
  // use the "most specific" common name, which is the last one in the list.
  X509_NAME* name = X509_get_subject_name(cert);
  int i = -1;
  ASN1_STRING* common_name = 0;
  while ((i = X509_NAME_get_index_by_NID(name, NID_commonName, i)) >= 0) {
    X509_NAME_ENTRY* name_entry = X509_NAME_get_entry(name, i);
    common_name = X509_NAME_ENTRY_get_data(name_entry);
  }
  if (common_name && common_name->data && common_name->length) {
    Slice common_name_slice(common_name->data, common_name->length);
    VLOG_WITH_PREFIX(4) << "Common name: " << common_name_slice.ToBuffer() << " vs "
                        << Remote().address() << "/" << remote_hostname_;
    if (FLAGS_dump_certificate_entries) {
      certificate_entries_.push_back(Format("CN:$0", common_name_slice.ToBuffer()));
    }
    if (common_name_slice == Remote().address().to_string() ||
        MatchPattern(common_name_slice, remote_hostname_)) {
      return true;
    }
  }

  VLOG_WITH_PREFIX(4) << "Nothing suitable for " << Remote().address() << "/" << remote_hostname_;
  return false;
}

} // namespace

const Protocol* SecureStreamProtocol() {
  static Protocol result("tcps");
  return &result;
}

StreamFactoryPtr SecureStreamFactory(
    StreamFactoryPtr lower_layer_factory, const MemTrackerPtr& buffer_tracker,
    SecureContext* context) {
  class SecureStreamFactory : public StreamFactory {
   public:
    SecureStreamFactory(
        StreamFactoryPtr lower_layer_factory, const MemTrackerPtr& buffer_tracker,
        SecureContext* context)
        : lower_layer_factory_(std::move(lower_layer_factory)), buffer_tracker_(buffer_tracker),
          context_(context) {
    }

   private:
    std::unique_ptr<Stream> Create(const StreamCreateData& data) override {
      auto receive_buffer_size = data.socket->GetReceiveBufferSize();
      if (!receive_buffer_size.ok()) {
        LOG(WARNING) << "Secure stream failure: " << receive_buffer_size.status();
        receive_buffer_size = 256_KB;
      }
      auto lower_stream = lower_layer_factory_->Create(data);
      return std::make_unique<SecureStream>(
          *context_, std::move(lower_stream), *receive_buffer_size, buffer_tracker_, data);
    }

    StreamFactoryPtr lower_layer_factory_;
    MemTrackerPtr buffer_tracker_;
    SecureContext* context_;
  };

  return std::make_shared<SecureStreamFactory>(
      std::move(lower_layer_factory), buffer_tracker, context);
}

} // namespace rpc
} // namespace yb
