// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/third_party/quiche/src/quic/core/http/quic_receive_control_stream.h"

#include <utility>

#include "net/third_party/quiche/src/quic/core/http/http_constants.h"
#include "net/third_party/quiche/src/quic/core/http/http_decoder.h"
#include "net/third_party/quiche/src/quic/core/http/quic_spdy_session.h"
#include "net/third_party/quiche/src/quic/core/quic_types.h"
#include "net/third_party/quiche/src/common/platform/api/quiche_string_piece.h"

namespace quic {

// Visitor of HttpDecoder that passes data frame to QuicSpdyStream and closes
// the connection on unexpected frames.
class QuicReceiveControlStream::HttpDecoderVisitor
    : public HttpDecoder::Visitor {
 public:
  explicit HttpDecoderVisitor(QuicReceiveControlStream* stream)
      : stream_(stream) {}
  HttpDecoderVisitor(const HttpDecoderVisitor&) = delete;
  HttpDecoderVisitor& operator=(const HttpDecoderVisitor&) = delete;

  void OnError(HttpDecoder* decoder) override {
    stream_->session()->connection()->CloseConnection(
        decoder->error(), decoder->error_detail(),
        ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  }

  bool OnCancelPushFrame(const CancelPushFrame& /*frame*/) override {
    CloseConnectionOnWrongFrame("Cancel Push");
    return false;
  }

  bool OnMaxPushIdFrame(const MaxPushIdFrame& frame) override {
    if (stream_->session()->perspective() == Perspective::IS_SERVER) {
      QuicSpdySession* spdy_session =
          static_cast<QuicSpdySession*>(stream_->session());
      spdy_session->SetMaxAllowedPushId(frame.push_id);
      return true;
    }
    CloseConnectionOnWrongFrame("Max Push Id");
    return false;
  }

  bool OnGoAwayFrame(const GoAwayFrame& frame) override {
    QuicSpdySession* spdy_session =
        static_cast<QuicSpdySession*>(stream_->session());
    if (spdy_session->perspective() == Perspective::IS_SERVER) {
      CloseConnectionOnWrongFrame("Go Away");
      return false;
    }
    spdy_session->OnHttp3GoAway(frame.stream_id);
    return true;
  }

  bool OnSettingsFrameStart(QuicByteCount header_length) override {
    return stream_->OnSettingsFrameStart(header_length);
  }

  bool OnSettingsFrame(const SettingsFrame& frame) override {
    return stream_->OnSettingsFrame(frame);
  }

  bool OnDuplicatePushFrame(const DuplicatePushFrame& /*frame*/) override {
    CloseConnectionOnWrongFrame("Duplicate Push");
    return false;
  }

  bool OnDataFrameStart(QuicByteCount /*header_length*/) override {
    CloseConnectionOnWrongFrame("Data");
    return false;
  }

  bool OnDataFramePayload(quiche::QuicheStringPiece /*payload*/) override {
    CloseConnectionOnWrongFrame("Data");
    return false;
  }

  bool OnDataFrameEnd() override {
    CloseConnectionOnWrongFrame("Data");
    return false;
  }

  bool OnHeadersFrameStart(QuicByteCount /*header_length*/) override {
    CloseConnectionOnWrongFrame("Headers");
    return false;
  }

  bool OnHeadersFramePayload(quiche::QuicheStringPiece /*payload*/) override {
    CloseConnectionOnWrongFrame("Headers");
    return false;
  }

  bool OnHeadersFrameEnd() override {
    CloseConnectionOnWrongFrame("Headers");
    return false;
  }

  bool OnPushPromiseFrameStart(QuicByteCount /*header_length*/) override {
    CloseConnectionOnWrongFrame("Push Promise");
    return false;
  }

  bool OnPushPromiseFramePushId(PushId /*push_id*/,
                                QuicByteCount /*push_id_length*/) override {
    CloseConnectionOnWrongFrame("Push Promise");
    return false;
  }

  bool OnPushPromiseFramePayload(
      quiche::QuicheStringPiece /*payload*/) override {
    CloseConnectionOnWrongFrame("Push Promise");
    return false;
  }

  bool OnPushPromiseFrameEnd() override {
    CloseConnectionOnWrongFrame("Push Promise");
    return false;
  }

  bool OnPriorityUpdateFrameStart(QuicByteCount /*header_length*/) override {
    // TODO(b/147306124): Implement.
    return true;
  }

  bool OnPriorityUpdateFrame(const PriorityUpdateFrame& /*frame*/) override {
    // TODO(b/147306124): Implement.
    return true;
  }

  bool OnUnknownFrameStart(uint64_t /* frame_type */,
                           QuicByteCount /* header_length */) override {
    // Ignore unknown frame types.
    return true;
  }

  bool OnUnknownFramePayload(quiche::QuicheStringPiece /* payload */) override {
    // Ignore unknown frame types.
    return true;
  }

  bool OnUnknownFrameEnd() override {
    // Ignore unknown frame types.
    return true;
  }

 private:
  void CloseConnectionOnWrongFrame(quiche::QuicheStringPiece frame_type) {
    stream_->session()->connection()->CloseConnection(
        QUIC_HTTP_FRAME_UNEXPECTED_ON_CONTROL_STREAM,
        quiche::QuicheStrCat(frame_type, " frame received on control stream"),
        ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  }

  QuicReceiveControlStream* stream_;
};

QuicReceiveControlStream::QuicReceiveControlStream(PendingStream* pending)
    : QuicStream(pending, READ_UNIDIRECTIONAL, /*is_static=*/true),
      settings_frame_received_(false),
      http_decoder_visitor_(std::make_unique<HttpDecoderVisitor>(this)),
      decoder_(http_decoder_visitor_.get()) {
  sequencer()->set_level_triggered(true);
}

QuicReceiveControlStream::~QuicReceiveControlStream() {}

void QuicReceiveControlStream::OnStreamReset(
    const QuicRstStreamFrame& /*frame*/) {
  // TODO(renjietang) Change the error code to H/3 specific
  // HTTP_CLOSED_CRITICAL_STREAM.
  session()->connection()->CloseConnection(
      QUIC_INVALID_STREAM_ID, "Attempt to reset receive control stream",
      ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
}

void QuicReceiveControlStream::OnDataAvailable() {
  iovec iov;
  while (!reading_stopped() && decoder_.error() == QUIC_NO_ERROR &&
         sequencer()->GetReadableRegion(&iov)) {
    DCHECK(!sequencer()->IsClosed());

    QuicByteCount processed_bytes = decoder_.ProcessInput(
        reinterpret_cast<const char*>(iov.iov_base), iov.iov_len);
    sequencer()->MarkConsumed(processed_bytes);

    if (!session()->connection()->connected()) {
      return;
    }

    // The only reason QuicReceiveControlStream pauses HttpDecoder is an error,
    // in which case the connection would have already been closed.
    DCHECK_EQ(iov.iov_len, processed_bytes);
  }
}

bool QuicReceiveControlStream::OnSettingsFrameStart(
    QuicByteCount /* header_length */) {
  if (settings_frame_received_) {
    // TODO(renjietang): Change error code to HTTP_UNEXPECTED_FRAME.
    session()->connection()->CloseConnection(
        QUIC_INVALID_STREAM_ID, "Settings frames are received twice.",
        ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
    return false;
  }

  settings_frame_received_ = true;

  return true;
}

bool QuicReceiveControlStream::OnSettingsFrame(const SettingsFrame& settings) {
  QUIC_DVLOG(1) << "Control Stream " << id()
                << " received settings frame: " << settings;
  QuicSpdySession* spdy_session = static_cast<QuicSpdySession*>(session());
  if (spdy_session->debug_visitor() != nullptr) {
    spdy_session->debug_visitor()->OnSettingsFrameReceived(settings);
  }
  for (const auto& setting : settings.values) {
    spdy_session->OnSetting(setting.first, setting.second);
  }
  return true;
}

}  // namespace quic
