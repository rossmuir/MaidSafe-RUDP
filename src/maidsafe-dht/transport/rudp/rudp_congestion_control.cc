/* Copyright (c) 2010 maidsafe.net limited
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.
    * Neither the name of the maidsafe.net limited nor the names of its
    contributors may be used to endorse or promote products derived from this
    software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// Author: Christopher M. Kohlhoff (chris at kohlhoff dot com)

#include <algorithm>
#include <cmath>

#include "rudp_congestion_control.h"

namespace bptime = boost::posix_time;

namespace maidsafe {

namespace transport {

static const bptime::time_duration kSynPeriod = bptime::milliseconds(10);

RudpCongestionControl::RudpCongestionControl()
  : slow_start_phase_(true),
    round_trip_time_(0),
    round_trip_time_variance_(0),
    packets_receiving_rate_(0),
    estimated_link_capacity_(0),
    send_window_size_(RudpParameters::kDefaultWindowSize),
    receive_window_size_(RudpParameters::kDefaultWindowSize),
    send_data_size_(RudpParameters::kDefaultDataSize),
    send_delay_(RudpParameters::kDefaultSendDelay),
    send_timeout_(RudpParameters::kDefaultSendTimeOut),
    receive_timeout_(RudpParameters::kDefaultReceiveTimeOut),
    ack_delay_(bptime::milliseconds(10)),
    ack_timeout_(RudpParameters::kDefaultAckTimeOut),
    ack_interval_(16),
    lost_packets_(0),
    corrupted_packets_(0),
    peer_connection_type_(0),
    allowed_lost_(0) {
}

void RudpCongestionControl::OnOpen(boost::uint32_t send_seqnum,
                                   boost::uint32_t receive_seqnum) {
}

void RudpCongestionControl::OnClose() {
}

void RudpCongestionControl::OnDataPacketSent(boost::uint32_t seqnum) {
}

void RudpCongestionControl::OnDataPacketReceived(boost::uint32_t seqnum) {
  bptime::ptime now = RudpTickTimer::Now();

  if ((seqnum % 16 == 1) && !arrival_times_.empty()) {
    // The pushed in interval is the interval between every 16 arrived packets
    packet_pair_intervals_.push_back(now - arrival_times_.back());
    while (packet_pair_intervals_.size() > kMaxPacketPairIntervals)
      packet_pair_intervals_.pop_front();
  }

  arrival_times_.push_back(now);
  while (arrival_times_.size() > kMaxArrivalTimes)
    arrival_times_.pop_front();
}

void RudpCongestionControl::OnGenerateAck(boost::uint32_t seqnum) {
  // Need to have received at least 8 packets to calculate receiving rate.
  if (arrival_times_.size() <= 8)
    return;

  // Calculate all packet arrival intervals.
  std::vector<boost::uint64_t> intervals;
  for (auto iter = arrival_times_.begin() + 1;
       iter != arrival_times_.end(); ++iter)
    intervals.push_back((*iter - *(iter - 1)).total_microseconds());

  // Find the median packet arrival interval.
  std::sort(intervals.begin(), intervals.end());
  boost::uint64_t median = intervals[intervals.size() / 2];

  // Calculate average of all intervals in range (median / 8) to (median * 8).
  size_t num_valid_intervals = 0;
  boost::uint64_t total = 0;
  for (auto iter = intervals.begin(); iter != intervals.end(); ++iter)
    if ((median / 8 <= *iter) && (*iter <= median * 8))
      ++num_valid_intervals, total += *iter;

  // Determine packet arrival speed only if we had more than 8 valid values.
  if ((total > 0) && (num_valid_intervals > 8))
    packets_receiving_rate_ = 1000000 * num_valid_intervals / total;
  else
    packets_receiving_rate_ = 0;

  // Need to have recorded some packet pair intervals to be able to calculate
  // the estimated link capacity.
  if (!packet_pair_intervals_.empty()) {
    // Calculate the estimated link capacity by determining the median of the
    // packet pair intervals, and from that determining the number of packets
    // per second.
    std::vector<boost::uint64_t> intervals;
    for (auto iter = packet_pair_intervals_.begin();
        iter != packet_pair_intervals_.end(); ++iter)
      intervals.push_back(iter->total_microseconds());
    std::sort(intervals.begin(), intervals.end());
    boost::uint64_t median = intervals[intervals.size() / 2];
    estimated_link_capacity_ = (median > 0) ? (1000000 / median) : 0;
  }

  // TODO (qi.ma@maidsafe.net) : The receive_window_size shall be based on the
  // local processing power, i.e. the reading speed of the data flow
  receive_window_size_ = RudpParameters::kMaximumWindowSize;
//   receive_window_size_ = (packets_receiving_rate_ *
//                           round_trip_time_) / 1000000;
//   // The speed of generating Ack Packets shall be considered
//   receive_window_size_ *= (1000 /
//                            RudpParameters::kAckInterval.total_milliseconds());
//   receive_window_size_ = std::max(receive_window_size_,
//                                   RudpParameters::kDefaultWindowSize);
//   receive_window_size_ = std::min(receive_window_size_,
//                                   RudpParameters::kMaximumWindowSize);
  // TODO calculate SND (send_delay_).
}

void RudpCongestionControl::OnAck(boost::uint32_t seqnum) {
}

void RudpCongestionControl::OnAck(boost::uint32_t seqnum,
                                  boost::uint32_t round_trip_time,
                                  boost::uint32_t round_trip_time_variance,
                                  boost::uint32_t available_buffer_size,
                                  boost::uint32_t packets_receiving_rate,
                                  boost::uint32_t estimated_link_capacity) {
  round_trip_time_ = round_trip_time;
  round_trip_time_variance_ = round_trip_time_variance;

  ack_delay_ = bptime::microseconds(UINT64_C(4) * round_trip_time_);
  ack_delay_ += bptime::microseconds(round_trip_time_variance_);
  ack_delay_ += kSynPeriod;

  if (packets_receiving_rate) {
    boost::uint64_t tmp = packets_receiving_rate_ * UINT64_C(7);
    tmp = (tmp + packets_receiving_rate) / 8;
    packets_receiving_rate_ = static_cast<boost::uint32_t>(tmp);
  }

  if (estimated_link_capacity) {
    boost::uint64_t tmp = estimated_link_capacity_ * UINT64_C(7);
    tmp = (tmp + estimated_link_capacity) / 8;
    estimated_link_capacity_ = static_cast<boost::uint32_t>(tmp);
  }
  // Each time an ack packet received, we check whether during this interval,
  // any packet reported to be lost or corrupted. If none, increase size,
  // otherwise decrease size
  if ((corrupted_packets_ + lost_packets_) > AllowedLost()) {
    send_data_size_ = 0.9 * send_data_size_;
    send_data_size_ = std::max(RudpParameters::kDefaultDataSize,
                               send_data_size_);
  } else {
    send_data_size_ = 1.5 * send_data_size_;
    send_data_size_ = std::min(RudpParameters::kMaxDataSize,
                               send_data_size_);
  }
  corrupted_packets_ = 0;
  lost_packets_ = 0;
  // If the other side still has some available buffer size, then the speed
  // can be increased this side. Otherwise, the sender's speed shall be reduced
  // To prevent osillator:
  //    a minum margin of 8 * kMaxDataSize for increase is taken
  //    a step of 20% for reducing window size is defined
  if (available_buffer_size > (8 * send_data_size_)) {
    send_window_size_ += (available_buffer_size + 1) /
                         RudpParameters::kMaxDataSize;
    send_window_size_ = std::min(send_window_size_,
                                 RudpParameters::kMaximumWindowSize);
  } else {
    send_window_size_ = 0.9 * send_window_size_;
    send_window_size_ = std::max(send_window_size_,
                                 RudpParameters::kDefaultWindowSize);
  }
}

void RudpCongestionControl::OnNegativeAck(boost::uint32_t seqnum) {
  ++corrupted_packets_;
}

void RudpCongestionControl::OnSendTimeout(boost::uint32_t seqnum) {
  ++lost_packets_;
}

void RudpCongestionControl::OnAckOfAck(boost::uint32_t round_trip_time) {
  boost::uint32_t diff = (round_trip_time < round_trip_time_) ?
                         (round_trip_time_ - round_trip_time) :
                         (round_trip_time - round_trip_time_);

  boost::uint32_t tmp = round_trip_time_ * UINT64_C(7);
  tmp = (tmp + round_trip_time) / 8;
  round_trip_time_ = static_cast<boost::uint32_t>(tmp);

  tmp = round_trip_time_variance_ * UINT64_C(3);
  tmp = (tmp + diff) / 4;
  round_trip_time_variance_ = static_cast<boost::uint32_t>(tmp);

  ack_delay_ = bptime::microseconds(UINT64_C(4) * round_trip_time_);
  ack_delay_ += bptime::microseconds(round_trip_time_variance_);
  ack_delay_ += kSynPeriod;
}

void RudpCongestionControl::SetPeerConnectionType(uint32_t connection_type) {
  peer_connection_type_ = connection_type;
  boost::uint32_t local_connection_type = RudpParameters::kConnectionType;
  boost::uint32_t worst_connection_type =
      std::min (peer_connection_type_, local_connection_type);
  if (worst_connection_type <= RudpParameters::kWireless) {
    allowed_lost_ = 5;
  } else if (worst_connection_type <= RudpParameters::kE1) {
    allowed_lost_ = 2;
  } else if (worst_connection_type <= RudpParameters::k1GEthernet) {
    allowed_lost_ = 1;
  }
}

size_t RudpCongestionControl::AllowedLost() const {
  return allowed_lost_;
}

boost::uint32_t RudpCongestionControl::RoundTripTime() const {
  return round_trip_time_;
}

boost::uint32_t RudpCongestionControl::RoundTripTimeVariance() const {
  return round_trip_time_variance_;
}

boost::uint32_t RudpCongestionControl::PacketsReceivingRate() const {
  return packets_receiving_rate_;
}

boost::uint32_t RudpCongestionControl::EstimatedLinkCapacity() const {
  return estimated_link_capacity_;
}

size_t RudpCongestionControl::SendWindowSize() const {
  return send_window_size_;
}

size_t RudpCongestionControl::ReceiveWindowSize() const {
  return receive_window_size_;
}

size_t RudpCongestionControl::SendDataSize() const {
  return send_data_size_;
}

boost::uint32_t RudpCongestionControl::BestReadBufferSize() {
  return receive_window_size_ * RudpParameters::kMaxDataSize;
}

boost::posix_time::time_duration RudpCongestionControl::SendDelay() const {
  return send_delay_;
}

boost::posix_time::time_duration RudpCongestionControl::SendTimeout() const {
  return send_timeout_;
}

boost::posix_time::time_duration RudpCongestionControl::ReceiveTimeout() const {
  return receive_timeout_;
}

boost::posix_time::time_duration RudpCongestionControl::AckDelay() const {
  return ack_delay_;
}

boost::posix_time::time_duration RudpCongestionControl::AckTimeout() const {
  return ack_timeout_;
}

// boost::uint32_t RudpCongestionControl::AckInterval() const {
//   return ack_interval_;
// }

boost::posix_time::time_duration RudpCongestionControl::AckInterval() const {
  return RudpParameters::kAckInterval;
}

}  // namespace transport

}  // namespace maidsafe
