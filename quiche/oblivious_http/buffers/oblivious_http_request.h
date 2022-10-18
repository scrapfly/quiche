#ifndef QUICHE_OBLIVIOUS_HTTP_BUFFERS_OBLIVIOUS_HTTP_REQUEST_H_
#define QUICHE_OBLIVIOUS_HTTP_BUFFERS_OBLIVIOUS_HTTP_REQUEST_H_

#include <memory>
#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "quiche/oblivious_http/common/oblivious_http_header_key_config.h"

namespace quiche {
// 1. Handles client side encryption of the payload that will subsequently be
// added to HTTP POST body and passed on to Relay.
// 2. Handles server side decryption of the payload received in HTTP POST body
// from Relay.
// https://www.ietf.org/archive/id/draft-ietf-ohai-ohttp-03.html#name-encapsulation-of-requests
class QUICHE_EXPORT_PRIVATE ObliviousHttpRequest {
 public:
  // Holds the HPKE related data received from request. This context is created
  // during request processing, and subsequently passed into response handling
  // in `ObliviousHttpResponse`.
  class Context {
   public:
    ~Context() = default;

   private:
    explicit Context(bssl::UniquePtr<EVP_HPKE_CTX> hpke_context,
                     std::string encapsulated_key);

    // All accessors must be friends to read `Context`.
    friend class ObliviousHttpRequest;
    friend class ObliviousHttpResponse;
    // Tests which need access.
    friend class
        ObliviousHttpRequest_TestDecapsulateWithSpecAppendixAExample_Test;
    friend class ObliviousHttpRequest_TestEncapsulatedRequestStructure_Test;
    friend class
        ObliviousHttpRequest_TestEncapsulatedOhttpEncryptedPayload_Test;
    friend class ObliviousHttpRequest_TestDeterministicSeededOhttpRequest_Test;
    friend class ObliviousHttpResponse_EndToEndTestForResponse_Test;
    friend class ObliviousHttpResponse_TestEncapsulateWithQuicheRandom_Test;

    bssl::UniquePtr<EVP_HPKE_CTX> hpke_context_;
    const std::string encapsulated_key_;
  };
  // Parse the OHTTP request from the given `encrypted_data`.
  // On success, returns obj that callers will use to `GetPlaintextData`.
  // Generic Usecase : server-side calls this method in the context of Request.
  static absl::StatusOr<ObliviousHttpRequest> CreateServerObliviousRequest(
      absl::string_view encrypted_data, const EVP_HPKE_KEY& gateway_key,
      const ObliviousHttpHeaderKeyConfig& ohttp_key_config);

  // Constructs an OHTTP request for the given `plaintext_payload`.
  // On success, returns obj that callers will use to `EncapsulateAndSerialize`
  // OHttp request.
  static absl::StatusOr<ObliviousHttpRequest> CreateClientObliviousRequest(
      absl::string_view plaintext_payload, absl::string_view hpke_public_key,
      const ObliviousHttpHeaderKeyConfig& ohttp_key_config);

  // Movable.
  ObliviousHttpRequest(ObliviousHttpRequest&& other) = default;
  ObliviousHttpRequest& operator=(ObliviousHttpRequest&& other) = default;

  ~ObliviousHttpRequest() = default;

  // Returns serialized OHTTP request bytestring.
  std::string EncapsulateAndSerialize() const;

  // Generic Usecase : server-side calls this method after Decapsulation using
  // `CreateServerObliviousRequest`.
  absl::string_view GetPlaintextData() const;

  // Oblivious HTTP request context is created after successful creation of
  // `this` object, and subsequently passed into the `ObliviousHttpResponse` for
  // followup response handling.
  std::shared_ptr<Context> oblivious_http_request_context() const {
    return oblivious_http_request_context_;
  }

 private:
  friend absl::StatusOr<ObliviousHttpRequest>
  CreateClientObliviousRequestWithSeedForTesting(
      absl::string_view plaintext_payload, absl::string_view hpke_public_key,
      const ObliviousHttpHeaderKeyConfig& ohttp_key_config,
      absl::string_view seed);

  explicit ObliviousHttpRequest(
      bssl::UniquePtr<EVP_HPKE_CTX> hpke_context, std::string encapsulated_key,
      const ObliviousHttpHeaderKeyConfig& ohttp_key_config,
      std::string req_ciphertext, std::string req_plaintext);

  static absl::StatusOr<ObliviousHttpRequest> EncapsulateWithSeed(
      absl::string_view plaintext_payload, absl::string_view hpke_public_key,
      const ObliviousHttpHeaderKeyConfig& ohttp_key_config,
      absl::string_view seed);

  std::shared_ptr<Context> oblivious_http_request_context_;
  ObliviousHttpHeaderKeyConfig key_config_;
  std::string request_ciphertext_;
  std::string request_plaintext_;
};

}  // namespace quiche

#endif  // QUICHE_OBLIVIOUS_HTTP_BUFFERS_OBLIVIOUS_HTTP_REQUEST_H_