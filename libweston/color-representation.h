/*
 * Copyright 2025 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#pragma once

#include <libweston/libweston.h>

#include "backend-drm/drm-kms-enums.h"

void
weston_reset_color_representation(struct weston_color_representation *color_rep);

struct weston_color_representation
weston_fill_color_representation(const struct weston_color_representation *color_rep_in,
				 const struct pixel_format_info *info);

enum weston_cr_comparison_flag {
	WESTON_CR_COMPARISON_FLAG_NONE = 0,
	WESTON_CR_COMPARISON_FLAG_IGNORE_ALPHA = 1,
	WESTON_CR_COMPARISON_FLAG_IGNORE_CHROMA_LOCATION = 2,
};

bool
weston_color_representation_equal(struct weston_color_representation *color_rep_A,
				  struct weston_color_representation *color_rep_B,
				  enum weston_cr_comparison_flag flags);

void
weston_get_color_representation_matrix(struct weston_compositor *compositor,
				       enum weston_color_matrix_coef coefficients,
				       enum weston_color_quant_range range,
				       struct weston_color_representation_matrix *cr_matrix);

bool
weston_surface_check_pending_color_representation_valid(const struct weston_surface *surface);

int
weston_compositor_enable_color_representation_protocol(struct weston_compositor *compositor);

struct weston_color_quant_range_info {
	enum weston_color_quant_range range;
	const char *name;
	enum wdrm_plane_color_range wdrm;
	uint32_t protocol_code;
};

const struct weston_color_quant_range_info *
weston_color_quant_range_info_from(enum weston_color_quant_range range);

const struct weston_color_quant_range_info *
weston_color_quant_range_info_from_protocol(uint32_t protocol_code);

struct weston_color_matrix_coef_info {
	enum weston_color_matrix_coef coefficients;
	const char *name;
	enum wdrm_plane_color_encoding wdrm;
	uint32_t protocol_code;
};

const struct weston_color_matrix_coef_info *
weston_color_matrix_coef_info_from(enum weston_color_matrix_coef coefficients);

const struct weston_color_matrix_coef_info *
weston_color_matrix_coef_info_from_protocol(uint32_t protocol_code);
