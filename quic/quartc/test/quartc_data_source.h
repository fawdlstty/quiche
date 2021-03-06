// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef QUICHE_QUIC_QUARTC_TEST_QUARTC_DATA_SOURCE_H_
#define QUICHE_QUIC_QUARTC_TEST_QUARTC_DATA_SOURCE_H_

#include "net/third_party/quiche/src/quic/core/crypto/quic_random.h"
#include "net/third_party/quiche/src/quic/core/quic_alarm.h"
#include "net/third_party/quiche/src/quic/core/quic_alarm_factory.h"
#include "net/third_party/quiche/src/quic/core/quic_bandwidth.h"
#include "net/third_party/quiche/src/quic/core/quic_time.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_clock.h"
#include "net/third_party/quiche/src/common/platform/api/quiche_string_piece.h"

namespace quic {
namespace test {

// Frames sent by a QuartcDataSource have a 20-byte header (4 bytes for the
// source id, 8 bytes for the sequence number, 8 bytes for the timestamp).
constexpr QuicByteCount kDataFrameHeaderSize = 20;

// Struct representing one frame of data sent by a QuartcDataSource.
struct ParsedQuartcDataFrame {
  // Parses the given data as a frame generated by QuartcDataSource.  Returns
  // true if parsing succeeds or false if parsing fails.
  static bool Parse(quiche::QuicheStringPiece data, ParsedQuartcDataFrame* out);

  // Note that a properly formatted, parseable frame always contains these three
  // header fields.
  int32_t source_id = -1;
  int64_t sequence_number = -1;
  QuicTime send_time = QuicTime::Zero();

  // Total size, including header and payload.
  QuicByteCount size = 0;
  std::string payload;
};

// Alarm-based source of random data to send.  QuartcDataSource is configured to
// generate new data at fixed intervals.
class QuartcDataSource {
 public:
  struct Config {
    // 32-bit ID for this data source.
    int32_t id = 0;

    // Minimum bandwidth allocated to this data source.
    QuicBandwidth min_bandwidth = QuicBandwidth::Zero();

    // Maximum bandwidth allocated to this data source.
    QuicBandwidth max_bandwidth = QuicBandwidth::Infinite();

    // Interval between frames for this data source.
    QuicTime::Delta frame_interval = QuicTime::Delta::FromMilliseconds(10);

    // Maximum size of frames produced by this source.  If this value is greater
    // than 0, the source may produce multiple frames with the same timestamp
    // rather than a single frame that is larger than this size.
    // If less than kDataFrameHeaderSize, the source produces frames of
    // kDataFrameHeaderSize.
    QuicByteCount max_frame_size = 0;
  };

  class Delegate {
   public:
    virtual ~Delegate() = default;

    virtual void OnDataProduced(const char* data, size_t length) = 0;
  };

  QuartcDataSource(const QuicClock* clock,
                   QuicAlarmFactory* alarm_factory,
                   QuicRandom* random,
                   const Config& config,
                   Delegate* delegate);

  void OnSendAlarm();

  // Allocates bandwidth to this source.  The source clamps the given value
  // between its configured min and max bandwidth, and returns any amount in
  // excess of its maximum allocation.
  QuicBandwidth AllocateBandwidth(QuicBandwidth bandwidth);

  // Whether the data source is enabled.  The data source only produces data
  // when enabled.  When first enabled, the data source starts sending
  // immediately.  When disabled, the data source stops sending immediately.
  bool Enabled() const;
  void SetEnabled(bool value);

  // Returns the sequence number of the last frame generated (or -1 if no frames
  // have been generated).
  int64_t sequence_number() const { return sequence_number_ - 1; }

 private:
  void GenerateFrame(QuicByteCount frame_size, QuicTime now);

  const QuicClock* clock_;
  QuicAlarmFactory* alarm_factory_;
  QuicRandom* random_;
  const Config config_;
  Delegate* delegate_;

  std::unique_ptr<QuicAlarm> send_alarm_;

  int64_t sequence_number_;
  QuicBandwidth allocated_bandwidth_;
  QuicTime last_send_time_;

  // Buffer for frames of data generated by the source.  The source writes each
  // frame into this buffer, then hands the delegate a pointer to it.  It's a
  // std::vector simply to make it quick and easy to resize if necessary (eg. if
  // |allocated_bandwidth_| increases and the frame size goes up.  Otherwise, it
  // would be a char[].
  std::vector<char> buffer_;
};

}  // namespace test
}  // namespace quic

#endif  // QUICHE_QUIC_QUARTC_TEST_QUARTC_DATA_SOURCE_H_
