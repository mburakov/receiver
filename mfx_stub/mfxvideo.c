/*
 * Copyright (C) 2024 Mikhail Burakov. This file is part of receiver.
 *
 * receiver is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * receiver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with receiver.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <mfxvideo.h>
#include <stdlib.h>
#include <string.h>

#include "bitstream.h"
#include "mfxsession_impl.h"

#define LENGTH(x) (sizeof(x) / sizeof *(x))

// Table 7-1 – NAL unit type codes and NAL unit type classes
enum NalUnitType {
  TRAIL_R = 1,
  BLA_W_LP = 16,
  IDR_W_RADL = 19,
  IDR_N_LP = 20,
  CRA_NUT = 21,
  RSV_IRAP_VCL23 = 23,
  VPS_NUT = 32,
  SPS_NUT = 33,
  PPS_NUT = 34,
  AUD_NUT = 35,
};

// Table 7-7
enum SliceType {
  P = 1,
  I = 2,
};

static uint64_t CeilLog2(uint64_t x) {
  return (uint64_t)(32 - __builtin_clz((uint32_t)(x - 1)));
}

// 7.3.1.2 NAL unit header syntax
static uint8_t ParseNaluHeader(struct Bitstream* nalu) {
  assert(BitstreamReadU(nalu, 1) == 0);  // forbidden_zero_bit
  uint64_t nal_unit_type = BitstreamReadU(nalu, 6);
  assert(BitstreamReadU(nalu, 6) == 0);  // nuh_layer_id
  assert(BitstreamReadU(nalu, 3) == 1);  // nuh_temporal_id_plus1
  return (uint8_t)nal_unit_type;
}

// 7.3.3 Profile, tier and level syntax
static void ParseProfileTierLevel(struct Bitstream* nalu) {
  assert(BitstreamReadU(nalu, 2) == 0);  // general_profile_space
  assert(BitstreamReadU(nalu, 1) == 0);  // general_tier_flag
  assert(BitstreamReadU(nalu, 5) == 1);  // general_profile_idc
  assert(BitstreamReadU(nalu, 32) ==
         3 << 29);                       // general_profile_compatibility_flag
  assert(BitstreamReadU(nalu, 1) == 1);  // general_progressive_source_flag
  assert(BitstreamReadU(nalu, 1) == 0);  // general_interlaced_source_flag
  assert(BitstreamReadU(nalu, 1) == 1);  // general_non_packed_constraint_flag
  assert(BitstreamReadU(nalu, 1) == 1);  // general_frame_only_constraint_flag
  assert(BitstreamReadU(nalu, 7) == 0);  // general_reserved_zero_7bits
  assert(BitstreamReadU(nalu, 1) ==
         0);  // general_one_picture_only_constraint_flag
  assert(BitstreamReadU(nalu, 35) == 0);   // general_reserved_zero_35bits
  assert(BitstreamReadU(nalu, 1) == 0);    // general_reserved_zero_bit
  assert(BitstreamReadU(nalu, 8) == 120);  // general_level_idc
}

// 7.3.7 Short-term reference picture set syntax
static void ParseStRefPicSet(struct Bitstream* nalu, uint64_t stRpsIdx) {
  if (stRpsIdx != 0) {
    assert(BitstreamReadU(nalu, 1) == 0);  // inter_ref_pic_set_prediction_flag
  }
  assert(BitstreamReadUE(nalu) == 1);    // num_negative_pics
  assert(BitstreamReadUE(nalu) == 0);    // num_positive_pics
  assert(BitstreamReadUE(nalu) == 0);    // delta_poc_s0_minus1
  assert(BitstreamReadU(nalu, 1) == 1);  // used_by_curr_pic_s0_flag
}

// E.2.1 VUI parameters syntax
static void ParseVuiParameters(struct Bitstream* nalu, mfxSession session) {
  assert(BitstreamReadU(nalu, 1) == 0);  // aspect_ratio_info_present_flag
  assert(BitstreamReadU(nalu, 1) == 0);  // overscan_info_present_flag
  assert(BitstreamReadU(nalu, 1) == 1);  // video_signal_type_present_flag

  // Table E.2 – Meaning of video_format
  assert(BitstreamReadU(nalu, 3) == 5);  // video_format
  assert(BitstreamReadU(nalu, 1) == 0);  // video_full_range_flag
  assert(BitstreamReadU(nalu, 1) == 1);  // colour_description_present_flag

  assert(BitstreamReadU(nalu, 8) == 2);  // colour_primaries
  assert(BitstreamReadU(nalu, 8) == 2);  // transfer_characteristics
  assert(BitstreamReadU(nalu, 8) == 6);  // matrix_coeffs

  assert(BitstreamReadU(nalu, 1) == 0);  // chroma_loc_info_present_flag
  assert(BitstreamReadU(nalu, 1) == 0);  // neutral_chroma_indication_flag
  assert(BitstreamReadU(nalu, 1) == 0);  // field_seq_flag
  assert(BitstreamReadU(nalu, 1) == 0);  // frame_field_info_present_flag

  bool default_display_window_flag = !!BitstreamReadU(nalu, 1);
  if (default_display_window_flag) {
    uint64_t def_disp_win_left_offset = BitstreamReadUE(nalu);
    uint64_t def_disp_win_right_offset = BitstreamReadUE(nalu);
    uint64_t def_disp_win_top_offset = BitstreamReadUE(nalu);
    uint64_t def_disp_win_bottom_offset = BitstreamReadUE(nalu);
    session->crop_rect[0] = (mfxU16)def_disp_win_left_offset;
    session->crop_rect[1] = (mfxU16)def_disp_win_top_offset;
    session->crop_rect[2] = (mfxU16)(session->ppb.pic_width_in_luma_samples -
                                     def_disp_win_right_offset);
    session->crop_rect[3] = (mfxU16)(session->ppb.pic_height_in_luma_samples -
                                     def_disp_win_bottom_offset);
  }

  assert(BitstreamReadU(nalu, 1) == 0);  // vui_timing_info_present_flag

  bool bitstream_restriction_flag = !!BitstreamReadU(nalu, 1);
  if (bitstream_restriction_flag) {
    assert(BitstreamReadU(nalu, 1) == 0);  // tiles_fixed_structure_flag
    assert(BitstreamReadU(nalu, 1) ==
           1);  // motion_vectors_over_pic_boundaries_flag
    assert(BitstreamReadU(nalu, 1) == 1);  // restricted_ref_pic_lists_flag
    assert(BitstreamReadUE(nalu) == 0);    // min_spatial_segmentation_idc
    assert(BitstreamReadUE(nalu) == 0);    // max_bytes_per_pic_denom
    assert(BitstreamReadUE(nalu) == 0);    // max_bits_per_min_cu_denom
    assert(BitstreamReadUE(nalu) == 15);   // log2_max_mv_length_horizontal
    assert(BitstreamReadUE(nalu) == 15);   // log2_max_mv_length_vertical
  }
}

// 7.3.2.2.1 General sequence parameter set RBSP syntax
static void ParseSps(struct Bitstream* nalu, mfxSession session) {
  assert(BitstreamReadU(nalu, 4) == 0);  // sps_video_parameter_set_id
  assert(BitstreamReadU(nalu, 3) == 0);  // sps_max_sub_layers_minus1
  assert(BitstreamReadU(nalu, 1) == 1);  // sps_temporal_id_nesting_flag
  ParseProfileTierLevel(nalu);
  assert(BitstreamReadUE(nalu) == 0);  // sps_seq_parameter_set_id
                                       //
  session->ppb.pic_fields.bits.chroma_format_idc =
      (uint32_t)BitstreamReadUE(nalu);
  assert(session->ppb.pic_fields.bits.chroma_format_idc == 1);
  session->ppb.pic_width_in_luma_samples = (uint16_t)BitstreamReadUE(nalu);
  session->ppb.pic_height_in_luma_samples = (uint16_t)BitstreamReadUE(nalu);
  bool conformance_window_flag = !!BitstreamReadU(nalu, 1);
  if (conformance_window_flag) {
    uint64_t conf_win_left_offset = BitstreamReadUE(nalu);
    uint64_t conf_win_right_offset = BitstreamReadUE(nalu);
    uint64_t conf_win_top_offset = BitstreamReadUE(nalu);
    uint64_t conf_win_bottom_offset = BitstreamReadUE(nalu);
    session->crop_rect[0] = (mfxU16)conf_win_left_offset;
    session->crop_rect[1] = (mfxU16)conf_win_top_offset;
    session->crop_rect[2] = (mfxU16)(session->ppb.pic_width_in_luma_samples -
                                     conf_win_right_offset);
    session->crop_rect[3] = (mfxU16)(session->ppb.pic_height_in_luma_samples -
                                     conf_win_bottom_offset);
  } else {
    session->crop_rect[2] = session->ppb.pic_width_in_luma_samples;
    session->crop_rect[3] = session->ppb.pic_height_in_luma_samples;
  }

  session->ppb.bit_depth_luma_minus8 = (uint8_t)BitstreamReadUE(nalu);
  session->ppb.bit_depth_chroma_minus8 = (uint8_t)BitstreamReadUE(nalu);
  session->ppb.log2_max_pic_order_cnt_lsb_minus4 =
      (uint8_t)BitstreamReadUE(nalu);
  assert(BitstreamReadU(nalu, 1) ==
         0);  // sps_sub_layer_ordering_info_present_flag

  session->ppb.sps_max_dec_pic_buffering_minus1 =
      (uint8_t)BitstreamReadUE(nalu);
  assert(BitstreamReadUE(nalu) == 0);  // sps_max_num_reorder_pics
  assert(BitstreamReadUE(nalu) == 0);  // sps_max_latency_increase_plus1

  session->ppb.log2_min_luma_coding_block_size_minus3 =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.log2_diff_max_min_luma_coding_block_size =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.log2_min_transform_block_size_minus2 =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.log2_diff_max_min_transform_block_size =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.max_transform_hierarchy_depth_inter =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.max_transform_hierarchy_depth_intra =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.pic_fields.bits.scaling_list_enabled_flag =
      (uint8_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.pic_fields.bits.scaling_list_enabled_flag == 0);

  session->ppb.pic_fields.bits.amp_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  session->ppb.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.slice_parsing_fields.bits
             .sample_adaptive_offset_enabled_flag == 1);
  session->ppb.pic_fields.bits.pcm_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.pic_fields.bits.pcm_enabled_flag == 0);

  // vvv weird vvv
  session->ppb.pcm_sample_bit_depth_luma_minus1 =
      (uint8_t)((1 << (session->ppb.bit_depth_luma_minus8 + 8)) - 1);
  session->ppb.pcm_sample_bit_depth_chroma_minus1 =
      (uint8_t)((1 << (session->ppb.bit_depth_chroma_minus8 + 8)) - 1);
  session->ppb.log2_min_pcm_luma_coding_block_size_minus3 = 253;
  // ^^^ weird ^^^

  session->ppb.num_short_term_ref_pic_sets = (uint8_t)BitstreamReadUE(nalu);
  for (uint8_t i = 0; i < session->ppb.num_short_term_ref_pic_sets; i++) {
    ParseStRefPicSet(nalu, i);
  }
  assert(BitstreamReadU(nalu, 1) == 0);  // long_term_ref_pics_present_flag

  session->ppb.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  session->ppb.pic_fields.bits.strong_intra_smoothing_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(BitstreamReadU(nalu, 1) == 1);  // vui_parameters_present_flag

  ParseVuiParameters(nalu, session);
  assert(BitstreamReadU(nalu, 1) == 0);  // sps_extension_present_flag
}

// 7.3.2.3.1 General picture parameter set RBSP syntax
static void ParsePps(struct Bitstream* nalu, mfxSession session) {
  assert(BitstreamReadUE(nalu) == 0);  // pps_pic_parameter_set_id
  assert(BitstreamReadUE(nalu) == 0);  // pps_seq_parameter_set_id

  session->ppb.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  session->ppb.slice_parsing_fields.bits.output_flag_present_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.slice_parsing_fields.bits.output_flag_present_flag == 0);
  session->ppb.num_extra_slice_header_bits = (uint8_t)BitstreamReadU(nalu, 3);
  assert(session->ppb.num_extra_slice_header_bits == 0);

  session->ppb.pic_fields.bits.sign_data_hiding_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  session->ppb.slice_parsing_fields.bits.cabac_init_present_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  session->ppb.num_ref_idx_l0_default_active_minus1 =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.num_ref_idx_l1_default_active_minus1 =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.init_qp_minus26 = (int8_t)BitstreamReadSE(nalu);
  session->ppb.pic_fields.bits.constrained_intra_pred_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  session->ppb.pic_fields.bits.transform_skip_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  session->ppb.pic_fields.bits.cu_qp_delta_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.pic_fields.bits.cu_qp_delta_enabled_flag == 0);

  session->ppb.pps_cb_qp_offset = (int8_t)BitstreamReadSE(nalu);
  session->ppb.pps_cr_qp_offset = (int8_t)BitstreamReadSE(nalu);
  session->ppb.slice_parsing_fields.bits
      .pps_slice_chroma_qp_offsets_present_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.slice_parsing_fields.bits
             .pps_slice_chroma_qp_offsets_present_flag == 0);

  session->ppb.pic_fields.bits.weighted_pred_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.pic_fields.bits.weighted_pred_flag == 0);
  session->ppb.pic_fields.bits.weighted_bipred_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.pic_fields.bits.weighted_bipred_flag == 0);

  session->ppb.pic_fields.bits.transquant_bypass_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  session->ppb.pic_fields.bits.tiles_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.pic_fields.bits.tiles_enabled_flag == 0);

  // vvv weird vvv
  session->ppb.pic_fields.bits.loop_filter_across_tiles_enabled_flag = 1;
  // ^^^ weird ^^^

  session->ppb.pic_fields.bits.entropy_coding_sync_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.pic_fields.bits.entropy_coding_sync_enabled_flag == 0);

  session->ppb.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  bool deblocking_filter_control_present_flag = !!BitstreamReadU(nalu, 1);
  if (deblocking_filter_control_present_flag) {
    session->ppb.slice_parsing_fields.bits
        .deblocking_filter_override_enabled_flag =
        (uint32_t)BitstreamReadU(nalu, 1);
    assert(session->ppb.slice_parsing_fields.bits
               .deblocking_filter_override_enabled_flag == 0);
    session->ppb.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag =
        (uint32_t)BitstreamReadU(nalu, 1);
    assert(session->ppb.slice_parsing_fields.bits
               .pps_disable_deblocking_filter_flag == 0);
    session->ppb.pps_beta_offset_div2 = (int8_t)BitstreamReadSE(nalu);
    session->ppb.pps_tc_offset_div2 = (int8_t)BitstreamReadSE(nalu);
  }

  assert(BitstreamReadU(nalu, 1) == 0);  // scaling_list_data_present_flag
  session->ppb.slice_parsing_fields.bits.lists_modification_present_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(
      session->ppb.slice_parsing_fields.bits.lists_modification_present_flag ==
      0);
  session->ppb.log2_parallel_merge_level_minus2 =
      (uint8_t)BitstreamReadUE(nalu);
  session->ppb.slice_parsing_fields.bits
      .slice_segment_header_extension_present_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->ppb.slice_parsing_fields.bits
             .slice_segment_header_extension_present_flag == 0);
  assert(BitstreamReadU(nalu, 1) == 0);  // pps_extension_present_flag
}

// 7.3.6.1 General slice segment header syntax
void ParseSliceSegmentHeader(struct Bitstream* nalu, mfxSession session,
                             enum NalUnitType nal_unit_type) {
  memset(&session->spb, 0, sizeof(session->spb));

  assert(BitstreamReadU(nalu, 1) == 1);  // first_slice_segment_in_pic_flag
  if (nal_unit_type >= BLA_W_LP && nal_unit_type <= RSV_IRAP_VCL23) {
    assert(BitstreamReadU(nalu, 1) == 0);  // no_output_of_prior_pics_flag
  }
  assert(BitstreamReadUE(nalu) == 0);  // slice_pic_parameter_set_id
  session->spb.LongSliceFlags.fields.slice_type =
      (uint32_t)BitstreamReadUE(nalu);

  if (nal_unit_type != IDR_W_RADL && nal_unit_type != IDR_N_LP) {
    size_t slice_pic_order_cnt_lsb_length =
        session->ppb.log2_max_pic_order_cnt_lsb_minus4 + 4;
    size_t slice_pic_order_cnt_lsb =
        BitstreamReadU(nalu, slice_pic_order_cnt_lsb_length);
    bool short_term_ref_pic_set_sps_flag = !!BitstreamReadU(nalu, 1);
    if (!short_term_ref_pic_set_sps_flag) {
      size_t offset = nalu->offset;
      size_t epb_count = nalu->epb_count;
      ParseStRefPicSet(nalu, session->ppb.num_short_term_ref_pic_sets);
      session->ppb.st_rps_bits =
          (uint32_t)(nalu->offset - offset -
                     ((nalu->epb_count - epb_count) << 3));
    } else if (session->ppb.num_short_term_ref_pic_sets > 1) {
      uint64_t short_term_ref_pic_set_idx_length =
          CeilLog2(session->ppb.num_short_term_ref_pic_sets);
      uint64_t short_term_ref_pic_set_idx =
          BitstreamReadU(nalu, (size_t)short_term_ref_pic_set_idx_length);
    }

    if (session->ppb.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag) {
      session->spb.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag =
          (uint32_t)BitstreamReadU(nalu, 1);
    }
  }

  session->spb.LongSliceFlags.fields.slice_sao_luma_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->spb.LongSliceFlags.fields.slice_sao_luma_flag == 1);
  session->spb.LongSliceFlags.fields.slice_sao_chroma_flag =
      (uint32_t)BitstreamReadU(nalu, 1);
  assert(session->spb.LongSliceFlags.fields.slice_sao_chroma_flag == 1);

  // vvv weird vvv
  session->spb.collocated_ref_idx = 0xff;
  session->spb.LongSliceFlags.fields.collocated_from_l0_flag = 1;
  session->spb.num_ref_idx_l0_active_minus1 =
      session->ppb.num_ref_idx_l0_default_active_minus1;
  session->spb.num_ref_idx_l1_active_minus1 =
      session->ppb.num_ref_idx_l1_default_active_minus1;
  // ^^^ weird ^^^

  if (session->spb.LongSliceFlags.fields.slice_type == P) {
    bool num_ref_idx_active_override_flag = !!BitstreamReadU(nalu, 1);
    if (num_ref_idx_active_override_flag) {
      session->spb.num_ref_idx_l0_active_minus1 =
          (uint8_t)BitstreamReadUE(nalu);
    }
    if (session->ppb.slice_parsing_fields.bits.cabac_init_present_flag) {
      session->spb.LongSliceFlags.fields.cabac_init_flag =
          (uint32_t)BitstreamReadU(nalu, 1);
    }
    if (session->spb.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag) {
      if ((session->spb.LongSliceFlags.fields.collocated_from_l0_flag &&
           session->spb.num_ref_idx_l0_active_minus1 > 0) ||
          (!session->spb.LongSliceFlags.fields.collocated_from_l0_flag &&
           session->spb.num_ref_idx_l1_active_minus1 > 0)) {
        session->spb.collocated_ref_idx = (uint8_t)BitstreamReadUE(nalu);
      }
    }
    session->spb.five_minus_max_num_merge_cand = (uint8_t)BitstreamReadUE(nalu);
  }
  session->spb.slice_qp_delta = (int8_t)BitstreamReadSE(nalu);
  if (session->ppb.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag &&
      (session->spb.LongSliceFlags.fields.slice_sao_luma_flag ||
       session->spb.LongSliceFlags.fields.slice_sao_chroma_flag)) {
    session->spb.LongSliceFlags.fields
        .slice_loop_filter_across_slices_enabled_flag =
        (uint32_t)BitstreamReadU(nalu, 1);
  }
  BitstreamByteAlign(nalu);
}

static bool UploadAndDecode(mfxSession session, const struct Bitstream* nalu) {
  VABufferID ppb_id;
  VAStatus status = vaCreateBuffer(
      session->display, session->context_id, VAPictureParameterBufferType,
      sizeof(session->ppb), 1, &session->ppb, &ppb_id);
  if (status != VA_STATUS_SUCCESS) return false;

  VABufferID spb_id;
  status = vaCreateBuffer(session->display, session->context_id,
                          VASliceParameterBufferType, sizeof(session->spb), 1,
                          &session->spb, &spb_id);
  if (status != VA_STATUS_SUCCESS) goto rollback_ppb_id;

  VABufferID sdb_id;
  status = vaCreateBuffer(session->display, session->context_id,
                          VASliceDataBufferType, (unsigned int)nalu->size, 1,
                          (void*)(uintptr_t)nalu->data, &sdb_id);
  if (status != VA_STATUS_SUCCESS) goto rollback_spb_id;

  status = vaBeginPicture(session->display, session->context_id,
                          session->ppb.CurrPic.picture_id);
  if (status != VA_STATUS_SUCCESS) goto rollback_sdb_id;

  VABufferID buffers[] = {ppb_id, spb_id, sdb_id};
  status = vaRenderPicture(session->display, session->context_id, buffers,
                           LENGTH(buffers));
  if (status != VA_STATUS_SUCCESS) goto rollback_sdb_id;

  status = vaEndPicture(session->display, session->context_id);
  if (status != VA_STATUS_SUCCESS) goto rollback_sdb_id;

  assert(vaDestroyBuffer(session->display, sdb_id) == VA_STATUS_SUCCESS);
  assert(vaDestroyBuffer(session->display, spb_id) == VA_STATUS_SUCCESS);
  assert(vaDestroyBuffer(session->display, ppb_id) == VA_STATUS_SUCCESS);
  return true;

rollback_sdb_id:
  assert(vaDestroyBuffer(session->display, sdb_id) == VA_STATUS_SUCCESS);
rollback_spb_id:
  assert(vaDestroyBuffer(session->display, spb_id) == VA_STATUS_SUCCESS);
rollback_ppb_id:
  assert(vaDestroyBuffer(session->display, ppb_id) == VA_STATUS_SUCCESS);
  return false;
}

mfxStatus MFXVideoCORE_SetFrameAllocator(mfxSession session,
                                         mfxFrameAllocator* allocator) {
  session->allocator = *allocator;
  return MFX_ERR_NONE;
}

mfxStatus MFXVideoCORE_SetHandle(mfxSession session, mfxHandleType type,
                                 mfxHDL hdl) {
  (void)type;
  session->display = hdl;
  return MFX_ERR_NONE;
}

mfxStatus MFXVideoCORE_SyncOperation(mfxSession session, mfxSyncPoint syncp,
                                     mfxU32 wait) {
  (void)session;
  (void)syncp;
  (void)wait;
  return MFX_ERR_NONE;
}

mfxStatus MFXVideoDECODE_Query(mfxSession session, mfxVideoParam* in,
                               mfxVideoParam* out) {
  (void)session;
  (void)in;
  (void)out;
  return MFX_ERR_NONE;
}

mfxStatus MFXVideoDECODE_DecodeHeader(mfxSession session, mfxBitstream* bs,
                                      mfxVideoParam* par) {
  (void)par;
  struct Bitstream bitstream = BitstreamCreate(bs->Data, bs->DataLength);
  for (struct Bitstream nalu; BitstreamAvail(&bitstream);) {
    if (!BitstreamReadNalu(&bitstream, &nalu)) {
      return MFX_ERR_UNSUPPORTED;
    }
    if (BitstreamReadFailed(&nalu)) {
      return MFX_ERR_UNSUPPORTED;
    }
    uint8_t nal_unit_type = ParseNaluHeader(&nalu);
    switch (nal_unit_type) {
      case SPS_NUT:
        ParseSps(&nalu, session);
        break;
      case PPS_NUT:
        ParsePps(&nalu, session);
        bs->Data += bitstream.offset >> 3;
        bs->DataLength -= bitstream.offset >> 3;
        return MFX_ERR_NONE;
      default:
        break;
    }
  }
  return MFX_ERR_NONE;
}

mfxStatus MFXVideoDECODE_Init(mfxSession session, mfxVideoParam* par) {
  (void)par;
  VAConfigID config_id;
  VAStatus status = vaCreateConfig(session->display, VAProfileHEVCMain,
                                   VAEntrypointVLD, NULL, 0, &config_id);
  if (status != VA_STATUS_SUCCESS) {
    return MFX_ERR_DEVICE_FAILED;
  }

  VAContextID context_id;
  mfxStatus result = MFX_ERR_DEVICE_FAILED;
  status = vaCreateContext(session->display, config_id,
                           session->ppb.pic_width_in_luma_samples,
                           session->ppb.pic_height_in_luma_samples,
                           VA_PROGRESSIVE, NULL, 0, &context_id);
  if (status != VA_STATUS_SUCCESS) {
    goto rollback_config_id;
  }

  mfxFrameAllocRequest request = {
      .Info.FourCC = MFX_FOURCC_NV12,
      .Info.Width = session->ppb.pic_width_in_luma_samples,
      .Info.Height = session->ppb.pic_height_in_luma_samples,
      .Info.ChromaFormat = MFX_CHROMAFORMAT_YUV420,
      .NumFrameSuggested = 3,
  };
  mfxFrameAllocResponse response;
  result =
      session->allocator.Alloc(session->allocator.pthis, &request, &response);
  if (result != MFX_ERR_NONE) {
    goto rollback_context_id;
  }

  mfxMemId* mids = calloc(response.NumFrameActual, sizeof(mfxMemId));
  if (!mids) {
    result = MFX_ERR_MEMORY_ALLOC;
    goto rollback_response;
  }

  session->config_id = config_id;
  session->context_id = context_id;
  session->mids = mids;
  session->mids_count = response.NumFrameActual;
  memcpy(mids, response.mids, response.NumFrameActual * sizeof(mfxMemId));
  return MFX_ERR_NONE;

rollback_response:
  assert(session->allocator.Free(session->allocator.pthis, &response) ==
         MFX_ERR_NONE);
rollback_context_id:
  assert(vaDestroyContext(session->display, context_id) == VA_STATUS_SUCCESS);
rollback_config_id:
  assert(vaDestroyConfig(session->display, config_id) == VA_STATUS_SUCCESS);
  return result;
}

mfxStatus MFXVideoDECODE_DecodeFrameAsync(mfxSession session, mfxBitstream* bs,
                                          mfxFrameSurface1* surface_work,
                                          mfxFrameSurface1** surface_out,
                                          mfxSyncPoint* syncp) {
  (void)syncp;
  struct Bitstream bitstream = BitstreamCreate(bs->Data, bs->DataLength);
  for (struct Bitstream nalu; BitstreamAvail(&bitstream);) {
    if (!BitstreamReadNalu(&bitstream, &nalu)) {
      return MFX_ERR_UNSUPPORTED;
    }
    if (BitstreamReadFailed(&nalu)) {
      return MFX_ERR_UNSUPPORTED;
    }
    uint8_t nal_unit_type = ParseNaluHeader(&nalu);
    if (nal_unit_type != TRAIL_R && nal_unit_type != IDR_W_RADL) continue;
    ParseSliceSegmentHeader(&nalu, session, nal_unit_type);

    ////////////////////////////////////////////////////////////////////////////

    mfxHDL psurface_current;
    mfxMemId mid_current =
        session->mids[session->global_frame_counter % session->mids_count];
    mfxStatus status = session->allocator.GetHDL(
        session->allocator.pthis, mid_current, &psurface_current);
    if (status != MFX_ERR_NONE) return status;

    if (nal_unit_type == IDR_W_RADL) session->local_frame_counter = 0;
    session->ppb.CurrPic.picture_id = *(VASurfaceID*)psurface_current;
    session->ppb.CurrPic.pic_order_cnt = (int32_t)session->local_frame_counter;
    for (size_t i = 0; i < LENGTH(session->ppb.ReferenceFrames); i++) {
      session->ppb.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
    }
    session->ppb.pic_fields.bits.NoPicReorderingFlag = 1;
    session->ppb.pic_fields.bits.NoBiPredFlag = 1;
    session->ppb.slice_parsing_fields.bits.RapPicFlag =
        BLA_W_LP <= nal_unit_type && nal_unit_type <= CRA_NUT;
    session->ppb.slice_parsing_fields.bits.IdrPicFlag =
        IDR_W_RADL <= nal_unit_type && nal_unit_type <= IDR_N_LP;
    session->ppb.slice_parsing_fields.bits.IntraPicFlag =
        BLA_W_LP <= nal_unit_type && nal_unit_type <= RSV_IRAP_VCL23;
    session->spb.slice_data_size = (uint32_t)nalu.size;
    session->spb.slice_data_offset = 0;
    session->spb.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    session->spb.slice_data_byte_offset =
        (uint32_t)((nalu.offset >> 3) - nalu.epb_count);
    for (size_t i = 0; i < LENGTH(session->spb.RefPicList); i++) {
      for (size_t j = 0; j < LENGTH(session->spb.RefPicList[i]); j++) {
        session->spb.RefPicList[i][j] = 0xff;
      }
    }
    session->spb.LongSliceFlags.fields.LastSliceOfPic = 1;
    session->spb.slice_data_num_emu_prevn_bytes = (uint16_t)nalu.epb_count;

    ////////////////////////////////////////////////////////////////////////////

    if (session->local_frame_counter) {
      mfxHDL psurface_prev;
      mfxMemId mid_prev =
          session
              ->mids[(session->global_frame_counter - 1) % session->mids_count];
      status = session->allocator.GetHDL(session->allocator.pthis, mid_prev,
                                         &psurface_prev);
      if (status != MFX_ERR_NONE) return status;
      session->ppb.ReferenceFrames[0].picture_id = *(VASurfaceID*)psurface_prev;
      session->ppb.ReferenceFrames[0].pic_order_cnt =
          (int32_t)session->local_frame_counter - 1;
      session->ppb.ReferenceFrames[0].flags =
          VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
      session->spb.RefPicList[0][0] = 0;
    }

    // TODO(mburakov): Does not seem to be used anywhere...
    (void)session->spb.entry_offset_to_subset_array;

    ////////////////////////////////////////////////////////////////////////////

    if (!UploadAndDecode(session, &nalu)) {
      return MFX_ERR_DEVICE_FAILED;
    }
    session->global_frame_counter++;
    session->local_frame_counter++;
    *surface_out = surface_work;
    *surface_work = (mfxFrameSurface1){
        .Info.CropX = session->crop_rect[0],
        .Info.CropY = session->crop_rect[1],
        .Info.CropW = session->crop_rect[2],
        .Info.CropH = session->crop_rect[3],
        .Data.MemId = mid_current,
    };
  }
  return MFX_ERR_NONE;
}
