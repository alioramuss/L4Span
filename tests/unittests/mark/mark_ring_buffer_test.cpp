// Unit tests for the PDCP SN ring buffer used by the L4Span marking entity
// (lib/mark/ring_buffer). The buffer stores one pdcp_sn_size_ts per PDCP SN and
// indexes by SN modulo its size, so the size must be a power of two.

#include "lib/mark/ring_buffer/ring_buffer.h"
#include <gtest/gtest.h>
#include <unordered_map>

namespace {

constexpr uint32_t default_size = 262144; // 18 bit PDCP SN space
constexpr uint32_t sn12_size    = 4096;   // 12 bit PDCP SN space

mark_utils::pdcp_sn_size_ts make_pkt(uint32_t sn, size_t size = 1500)
{
  mark_utils::pdcp_sn_size_ts pkt = {};
  pkt.pdcp_sn                     = sn;
  pkt.size                        = size;
  return pkt;
}

} // namespace

TEST(ring_buffer_test, when_element_is_added_then_it_can_be_read_back_by_sn)
{
  RingBuffer rb(sn12_size);
  rb.push_back(make_pkt(7, 1200));

  EXPECT_EQ(rb[7].pdcp_sn, 7U);
  EXPECT_EQ(rb[7].size, 1200U);
  EXPECT_EQ(rb.get_element(7).size, 1200U);
}

TEST(ring_buffer_test, when_elements_are_added_then_tail_tracks_the_last_one)
{
  RingBuffer rb(sn12_size);
  for (uint32_t sn = 0; sn < 10; ++sn) {
    rb.push_back(make_pkt(sn, 100 + sn));
  }

  EXPECT_EQ(rb.get_last_pdcp_ind(), 9U);
  EXPECT_EQ(rb.back().pdcp_sn, 9U);
  EXPECT_EQ(rb.back().size, 109U);
}

TEST(ring_buffer_test, when_sn_exceeds_size_then_index_wraps_modulo_size)
{
  RingBuffer rb(sn12_size);
  rb.push_back(make_pkt(sn12_size + 5, 999));

  // Stored in slot 5, and reachable through either SN.
  EXPECT_EQ(rb.get_last_pdcp_ind(), 5U);
  EXPECT_EQ(rb[5].size, 999U);
  EXPECT_EQ(rb[sn12_size + 5].size, 999U);
}

TEST(ring_buffer_test, when_sn_space_wraps_then_new_element_overwrites_old_slot)
{
  RingBuffer rb(sn12_size);
  rb.push_back(make_pkt(3, 111));
  rb.push_back(make_pkt(sn12_size + 3, 222));

  EXPECT_EQ(rb[3].size, 222U);
  EXPECT_EQ(rb.back().size, 222U);
}

TEST(ring_buffer_test, when_every_sn_in_a_12_bit_space_is_used_then_all_slots_are_distinct)
{
  RingBuffer rb(sn12_size);
  for (uint32_t sn = 0; sn < sn12_size; ++sn) {
    rb.push_back(make_pkt(sn, sn));
  }
  for (uint32_t sn = 0; sn < sn12_size; ++sn) {
    ASSERT_EQ(rb[sn].pdcp_sn, sn);
    ASSERT_EQ(rb[sn].size, sn);
  }
  EXPECT_EQ(rb.get_last_pdcp_ind(), sn12_size - 1);
}

TEST(ring_buffer_test, when_element_is_modified_through_reference_then_change_is_stored)
{
  // mark_entity_impl writes transmitted_time, cal_dequeue_rate etc. through the
  // returned reference, so it must alias the stored element.
  RingBuffer rb(sn12_size);
  rb.push_back(make_pkt(42));
  rb[42].cal_dequeue_rate = 12.5;
  rb.back().queue_delay   = 3.0;

  EXPECT_DOUBLE_EQ(rb.get_element(42).cal_dequeue_rate, 12.5);
  EXPECT_DOUBLE_EQ(rb[42].queue_delay, 3.0);
}

TEST(ring_buffer_test, when_18_bit_sn_space_is_used_then_high_sns_do_not_alias)
{
  RingBuffer rb(default_size);
  rb.push_back(make_pkt(sn12_size + 1, 10));
  rb.push_back(make_pkt(1, 20));

  // 4097 and 1 collide in a 12 bit buffer but not in an 18 bit one.
  EXPECT_EQ(rb[sn12_size + 1].size, 10U);
  EXPECT_EQ(rb[1].size, 20U);
}

TEST(ring_buffer_test, when_default_constructed_then_buffer_is_usable)
{
  RingBuffer rb;
  rb.push_back(make_pkt(100, 1500));

  EXPECT_EQ(rb.get_last_pdcp_ind(), 100U);
  EXPECT_EQ(rb[100].size, 1500U);
}

TEST(ring_buffer_test, when_default_constructed_inside_a_map_then_buffer_is_usable)
{
  // mark_entity_impl::drb_queue_update() does drb_pdcp_sn_ts[drb_id].push_back(...),
  // which default constructs the buffer if handle_feedback() has not created it yet.
  std::unordered_map<int, RingBuffer> buffers;
  buffers[1].push_back(make_pkt(5, 1400));

  EXPECT_EQ(buffers[1][5].size, 1400U);
}

TEST(ring_buffer_test, when_size_is_not_a_power_of_two_then_distinct_sns_do_not_collide)
{
  // A non power of two size falls back to the default size, so SN 0 and SN 8
  // must land in different slots.
  RingBuffer rb(1000);
  rb.push_back(make_pkt(0, 10));
  rb.push_back(make_pkt(8, 20));

  EXPECT_EQ(rb[0].pdcp_sn, 0U);
  EXPECT_EQ(rb[0].size, 10U);
  EXPECT_EQ(rb[8].size, 20U);
}

TEST(ring_buffer_test, when_size_is_zero_then_index_stays_inside_the_default_buffer)
{
  RingBuffer rb(0);
  rb.push_back(make_pkt(default_size + 3, 77));

  EXPECT_LT(rb.get_last_pdcp_ind(), default_size);
  EXPECT_EQ(rb[3].size, 77U);
}

TEST(ring_buffer_test, when_buffer_is_replaced_by_assignment_then_new_size_applies)
{
  // handle_feedback() does drb_pdcp_sn_ts[drb_id] = RingBuffer(MOD).
  std::unordered_map<int, RingBuffer> buffers;
  buffers[1] = RingBuffer(sn12_size);
  buffers[1].push_back(make_pkt(sn12_size + 2, 55));

  EXPECT_EQ(buffers[1].get_last_pdcp_ind(), 2U);
  EXPECT_EQ(buffers[1][2].size, 55U);
}
