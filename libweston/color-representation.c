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

#include "config.h"

#include "color-representation.h"

#include "libweston-internal.h"
#include "pixel-formats.h"
#include "shared/string-helpers.h"
#include "shared/weston-assert.h"
#include "shared/xalloc.h"

#include "color-representation-v1-server-protocol.h"

static const enum wp_color_representation_surface_v1_alpha_mode supported_alpha_modes[] = {
	WP_COLOR_REPRESENTATION_SURFACE_V1_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL,
};

struct coeffs_and_range {
	enum wp_color_representation_surface_v1_coefficients coefficients;
	enum wp_color_representation_surface_v1_range range;
};

#define CRS(x) WP_COLOR_REPRESENTATION_SURFACE_V1_ ##x
static const struct coeffs_and_range supported_coeffs_and_ranges[] = {
	{ CRS(COEFFICIENTS_IDENTITY), CRS(RANGE_FULL) },
	{ CRS(COEFFICIENTS_BT601), CRS(RANGE_LIMITED) },
	{ CRS(COEFFICIENTS_BT601), CRS(RANGE_FULL) },
	{ CRS(COEFFICIENTS_BT709), CRS(RANGE_LIMITED) },
	{ CRS(COEFFICIENTS_BT709), CRS(RANGE_FULL) },
	{ CRS(COEFFICIENTS_BT2020), CRS(RANGE_LIMITED) },
	{ CRS(COEFFICIENTS_BT2020), CRS(RANGE_FULL) },
};
#undef CRS

WL_EXPORT void
weston_reset_color_representation(struct weston_color_representation *color_rep)
{
	color_rep->alpha_mode = WESTON_ALPHA_MODE_PREMULTIPLIED_ELECTRICAL;
	color_rep->matrix_coefficients = WESTON_COLOR_MATRIX_COEF_UNSET;
	color_rep->quant_range = WESTON_COLOR_QUANT_RANGE_UNSET;
	color_rep->chroma_location = WESTON_YCBCR_CHROMA_LOCATION_UNSET;
}

WL_EXPORT struct weston_color_representation
weston_fill_color_representation(const struct weston_color_representation *color_rep_in,
				 const struct pixel_format_info *info)
{
	struct weston_color_representation color_rep;

	color_rep = *color_rep_in;

	if (color_rep.matrix_coefficients == WESTON_COLOR_MATRIX_COEF_UNSET) {
		if (info->color_model == COLOR_MODEL_YUV)
			color_rep.matrix_coefficients =
				WESTON_COLOR_MATRIX_COEF_BT709;
		else
			color_rep.matrix_coefficients =
				WESTON_COLOR_MATRIX_COEF_IDENTITY;
	}
	if (color_rep.quant_range == WESTON_COLOR_QUANT_RANGE_UNSET) {
		if (info->color_model == COLOR_MODEL_YUV)
			color_rep.quant_range = WESTON_COLOR_QUANT_RANGE_LIMITED;
		else
			color_rep.quant_range = WESTON_COLOR_QUANT_RANGE_FULL;
	}

	return color_rep;
}


WL_EXPORT bool
weston_color_representation_equal(struct weston_color_representation *color_rep_A,
				  struct weston_color_representation *color_rep_B,
				  enum weston_cr_comparison_flag flags)
{
	if (!(flags & WESTON_CR_COMPARISON_FLAG_IGNORE_ALPHA) &&
	    color_rep_A->alpha_mode != color_rep_B->alpha_mode)
		return false;

	if (!(flags & WESTON_CR_COMPARISON_FLAG_IGNORE_CHROMA_LOCATION) &&
	    color_rep_A->chroma_location != color_rep_B->chroma_location)
		return false;

	return (color_rep_A->matrix_coefficients == color_rep_B->matrix_coefficients &&
		color_rep_A->quant_range == color_rep_B->quant_range);
}

WL_EXPORT void
weston_get_color_representation_matrix(struct weston_compositor *compositor,
				       enum weston_color_matrix_coef coefficients,
				       enum weston_color_quant_range range,
				       struct weston_color_representation_matrix *cr_matrix)
{
	/* The values in this function are copied from Mesa and may not be
	 * optimal or correct in all cases. */

	if (range == WESTON_COLOR_QUANT_RANGE_FULL) {
		cr_matrix->offset =
			WESTON_VEC3F(0.0, 128.0 / 255.0, 128.0 / 255.0);

		switch(coefficients) {
		case WESTON_COLOR_MATRIX_COEF_BT601:
			cr_matrix->matrix =
				WESTON_MAT3F(1.0,  0.0,         1.402,
					     1.0, -0.34413629, -0.71413629,
					     1.0,  1.772,       0.0);
			return;
		case WESTON_COLOR_MATRIX_COEF_BT709:
			cr_matrix->matrix =
				WESTON_MAT3F(1.0,  0.0,         1.5748,
					     1.0, -0.18732427, -0.46812427,
					     1.0,  1.8556,      0.0);
			return;
		case WESTON_COLOR_MATRIX_COEF_BT2020:
			cr_matrix->matrix =
				WESTON_MAT3F(1.0,  0.0,         1.4746,
					     1.0, -0.16455313, -0.57139187,
					     1.0,  1.8814,      0.0);
			return;
		default:
			break;
		}
	} else if (range == WESTON_COLOR_QUANT_RANGE_LIMITED) {
		cr_matrix->offset =
			WESTON_VEC3F(16.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0);

		switch(coefficients) {
		case WESTON_COLOR_MATRIX_COEF_BT601:
			cr_matrix->matrix =
				WESTON_MAT3F(255.0 / 219.0,  0.0,         1.59602678,
					     255.0 / 219.0, -0.39176229, -0.81296764,
					     255.0 / 219.0,  2.01723214,  0.0);
			return;
		case WESTON_COLOR_MATRIX_COEF_BT709:
			cr_matrix->matrix =
				WESTON_MAT3F(255.0 / 219.0,  0.0,         1.79274107,
					     255.0 / 219.0, -0.21324861, -0.53290933,
					     255.0 / 219.0,  2.11240179,  0.0);
			return;
		case WESTON_COLOR_MATRIX_COEF_BT2020:
			cr_matrix->matrix =
				WESTON_MAT3F(255.0 / 219.0,  0.0,         1.67878795,
					     255.0 / 219.0, -0.18732610, -0.65046843,
					     255.0 / 219.0,  2.14177232,  0.0);
			return;
		default:
			break;
		}
	}

	weston_assert_not_reached(compositor,
		"unknown coefficients or range value");
}

bool
weston_surface_check_pending_color_representation_valid(
	const struct weston_surface *surface)
{
	const struct weston_surface_state *pend = &surface->pending;
	const struct weston_color_representation *cr =
		&pend->color_representation;
	struct weston_buffer *buffer = NULL;
	bool format_is_yuv;

	if (!surface->color_representation_resource)
		return true;

	if (cr->matrix_coefficients == WESTON_COLOR_MATRIX_COEF_UNSET &&
	    cr->quant_range == WESTON_COLOR_QUANT_RANGE_UNSET)
		return true;

	assert(cr->matrix_coefficients != WESTON_COLOR_MATRIX_COEF_UNSET &&
	       cr->quant_range != WESTON_COLOR_QUANT_RANGE_UNSET);

	if (pend->status & WESTON_SURFACE_DIRTY_BUFFER) {
		buffer = pend->buffer_ref.buffer;
	} else if (surface->buffer_ref.buffer) {
		buffer = surface->buffer_ref.buffer;
	}

	if (!buffer)
		return true;

	format_is_yuv = buffer->pixel_format->color_model == COLOR_MODEL_YUV;
	if ((format_is_yuv &&
	     cr->matrix_coefficients == WESTON_COLOR_MATRIX_COEF_IDENTITY) ||
	    (!format_is_yuv &&
	     cr->matrix_coefficients != WESTON_COLOR_MATRIX_COEF_IDENTITY)) {
		wl_resource_post_error(surface->color_representation_resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_PIXEL_FORMAT,
			"wp_color_representation_surface_v1@%"PRIu32" "
			"Buffer format %s not compatible "
			"with matrix coefficients %u",
			wl_resource_get_id(surface->resource),
			buffer->pixel_format->drm_format_name,
			cr->matrix_coefficients);
		return false;
	}

	return true;
}

static void
destroy_color_representation(struct wl_resource *resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(resource);

	if (!surface)
		return;

	surface->color_representation_resource = NULL;
	weston_reset_color_representation(&surface->pending.color_representation);
}

static void
cr_destroy(struct wl_client *client,
	   struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
cr_set_alpha_mode(struct wl_client *client,
		  struct wl_resource *resource,
		  uint32_t alpha_mode)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(resource);
	bool supported = false;

	if (!surface) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
			"wp_color_representation_surface_v1@%"PRIu32" "
			"The object is inert.",
			wl_resource_get_id(resource));
		return;
	}

	weston_assert_ptr_eq(surface->compositor,
			     surface->color_representation_resource, resource);

	for (unsigned int i = 0; i < ARRAY_LENGTH(supported_alpha_modes); i++) {
		if (supported_alpha_modes[i] == alpha_mode) {
			    supported = true;
			    break;
		}
	}
	if (!supported) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_ALPHA_MODE,
			"wp_color_representation_surface_v1@%"PRIu32" "
			"Invalid alpha mode (%u)",
			wl_resource_get_id(resource), alpha_mode);
		return;
	}

	surface->pending.color_representation.alpha_mode = alpha_mode;
}

static void
cr_set_coefficients_and_range(struct wl_client *client,
			      struct wl_resource *resource,
			      uint32_t coefficients,
			      uint32_t range)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(resource);
	struct weston_color_representation *color_representation;
	bool supported = false;

	if (!surface) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
			"wp_color_representation_surface_v1@%"PRIu32" "
			"The object is inert.",
			wl_resource_get_id(resource));
		return;
	}

	weston_assert_ptr_eq(surface->compositor,
			     surface->color_representation_resource, resource);

	color_representation = &surface->pending.color_representation;

	for (unsigned int i = 0; i < ARRAY_LENGTH(supported_coeffs_and_ranges); i++) {
		if (supported_coeffs_and_ranges[i].coefficients == coefficients &&
		    supported_coeffs_and_ranges[i].range == range) {
			    supported = true;
			    break;
		}
	}
	if (!supported) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_COEFFICIENTS,
			"wp_color_representation_surface_v1@%"PRIu32" "
			"Invalid coefficients (%u) or range (%u).",
			wl_resource_get_id(resource), coefficients, range);
		return;
	}

	switch (coefficients) {
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY:
		color_representation->matrix_coefficients = WESTON_COLOR_MATRIX_COEF_IDENTITY;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709:
		color_representation->matrix_coefficients = WESTON_COLOR_MATRIX_COEF_BT709;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601:
		color_representation->matrix_coefficients = WESTON_COLOR_MATRIX_COEF_BT601;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020:
		color_representation->matrix_coefficients = WESTON_COLOR_MATRIX_COEF_BT2020;
		break;
	default:
		weston_assert_not_reached(surface->compositor, "unsupported coefficients");
	}

	switch (range) {
	case WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL:
		color_representation->quant_range = WESTON_COLOR_QUANT_RANGE_FULL;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED:
		color_representation->quant_range = WESTON_COLOR_QUANT_RANGE_LIMITED;
		break;
	default:
		weston_assert_not_reached(surface->compositor, "unsupported range");
	}
}

static void
cr_set_chroma_location(struct wl_client *client,
		       struct wl_resource *resource,
		       uint32_t chroma_location)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(resource);
	struct weston_color_representation *color_representation;

	if (!surface) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_INERT,
			"wp_color_representation_surface_v1@%"PRIu32" "
			"The object is inert.",
			wl_resource_get_id(resource));
		return;
	}

	weston_assert_ptr_eq(surface->compositor,
			     surface->color_representation_resource, resource);

	color_representation = &surface->pending.color_representation;

	switch (chroma_location) {
	case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_0:
		color_representation->chroma_location = WESTON_YCBCR_CHROMA_LOCATION_TYPE_0;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_1:
		color_representation->chroma_location = WESTON_YCBCR_CHROMA_LOCATION_TYPE_1;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_2:
		color_representation->chroma_location = WESTON_YCBCR_CHROMA_LOCATION_TYPE_2;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_3:
		color_representation->chroma_location = WESTON_YCBCR_CHROMA_LOCATION_TYPE_3;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_4:
		color_representation->chroma_location = WESTON_YCBCR_CHROMA_LOCATION_TYPE_4;
		break;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_CHROMA_LOCATION_TYPE_5:
		color_representation->chroma_location = WESTON_YCBCR_CHROMA_LOCATION_TYPE_5;
		break;
	default:
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_SURFACE_V1_ERROR_CHROMA_LOCATION,
			"wp_color_representation_surface_v1@%"PRIu32" "
			"Invalid chroma location (%u).",
			wl_resource_get_id(resource),
			chroma_location);
		return;
	}
}

static const struct wp_color_representation_surface_v1_interface cr_implementation = {
	.destroy = cr_destroy,
	.set_alpha_mode = cr_set_alpha_mode,
	.set_coefficients_and_range = cr_set_coefficients_and_range,
	.set_chroma_location = cr_set_chroma_location,
};

static void
cr_manager_destroy(struct wl_client *client,
                   struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
cr_manager_get_surface(struct wl_client *client,
		       struct wl_resource *resource,
		       uint32_t id,
		       struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct wl_resource *color_representation_resource;

	if (surface->color_representation_resource) {
		wl_resource_post_error(resource,
			WP_COLOR_REPRESENTATION_MANAGER_V1_ERROR_SURFACE_EXISTS,
			"a color representation surface for that surface already exists");
		return;
	}

	color_representation_resource = wl_resource_create(client,
		&wp_color_representation_surface_v1_interface,
		wl_resource_get_version(resource), id);
	if (color_representation_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(color_representation_resource,
		&cr_implementation, surface, destroy_color_representation);

	surface->color_representation_resource = color_representation_resource;
}

static const struct wp_color_representation_manager_v1_interface
cr_manager_implementation = {
        .destroy = cr_manager_destroy,
	.get_surface = cr_manager_get_surface,
};

static void
bind_color_representation(struct wl_client *client, void *data, uint32_t version,
                          uint32_t id)
{
        struct wl_resource *resource;
        struct weston_compositor *compositor = data;

	resource = wl_resource_create(client,
		&wp_color_representation_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &cr_manager_implementation,
				       compositor, NULL);

	for (unsigned int i = 0; i < ARRAY_LENGTH(supported_alpha_modes); i++) {
		wp_color_representation_manager_v1_send_supported_alpha_mode(
			resource,
			supported_alpha_modes[i]);
	}

	for (unsigned int i = 0; i < ARRAY_LENGTH(supported_coeffs_and_ranges); i++) {
		wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
			resource,
			supported_coeffs_and_ranges[i].coefficients,
			supported_coeffs_and_ranges[i].range);
	}

	wp_color_representation_manager_v1_send_done(resource);
}

/** Advertise color-representation support
 *
 * Calling this initializes the color-representation protocol support, so that
 * wp_color_representation_manager_v1_interface will be advertised to clients.
 * Essentially it creates a global. Do not call this function multiple times in
 * the compositor's lifetime. There is no way to deinit explicitly, globals will
 * be reaped when the wl_display gets destroyed.
 *
 * \param compositor The compositor to init for.
 * \return Zero on success, -1 on failure.
 */
int
weston_compositor_enable_color_representation_protocol(struct weston_compositor *compositor)
{
        uint32_t version = 1;

	if (!(compositor->capabilities & WESTON_CAP_COLOR_REP)) {
		weston_log("Color representation not supported by renderer\n");
		return 0;
	}

        if (!wl_global_create(compositor->wl_display,
                              &wp_color_representation_manager_v1_interface,
                              version, compositor, bind_color_representation))
                return -1;

        return 0;
}

static const struct weston_color_matrix_coef_info color_matrix_coef_info_map[] = {
	{
		.coefficients = WESTON_COLOR_MATRIX_COEF_UNSET,
		.name = "unset",
		.wdrm = WDRM_PLANE_COLOR_ENCODING__COUNT,
	},
	{
		.coefficients = WESTON_COLOR_MATRIX_COEF_IDENTITY,
		.name = "default",
		.wdrm = WDRM_PLANE_COLOR_ENCODING__COUNT,
	},
	{
		.coefficients = WESTON_COLOR_MATRIX_COEF_BT601,
		.name = "BT.601",
		.wdrm = WDRM_PLANE_COLOR_ENCODING_BT601,
	},
	{
		.coefficients = WESTON_COLOR_MATRIX_COEF_BT709,
		.name = "BT.709",
		.wdrm = WDRM_PLANE_COLOR_ENCODING_BT709,
	},
	{
		.coefficients = WESTON_COLOR_MATRIX_COEF_BT2020,
		.name = "BT.2020",
		.wdrm = WDRM_PLANE_COLOR_ENCODING_BT2020,
	},
};

WL_EXPORT const struct weston_color_matrix_coef_info *
weston_color_matrix_coef_info_from(enum weston_color_matrix_coef coefficients)
{
	for (unsigned i = 0; i < ARRAY_LENGTH(color_matrix_coef_info_map); i++)
		if (color_matrix_coef_info_map[i].coefficients == coefficients)
			return &color_matrix_coef_info_map[i];

	return NULL;
}

static const struct weston_color_quant_range_info color_quant_range_info_map[] = {
	{ WESTON_COLOR_QUANT_RANGE_UNSET, "unset", WDRM_PLANE_COLOR_RANGE__COUNT },
	{ WESTON_COLOR_QUANT_RANGE_FULL, "full", WDRM_PLANE_COLOR_RANGE_FULL },
	{ WESTON_COLOR_QUANT_RANGE_LIMITED, "limited", WDRM_PLANE_COLOR_RANGE_LIMITED },
};

WL_EXPORT const struct weston_color_quant_range_info *
weston_color_quant_range_info_from(enum weston_color_quant_range range)
{
	for (unsigned i = 0; i < ARRAY_LENGTH(color_quant_range_info_map); i++)
		if (color_quant_range_info_map[i].range == range)
			return &color_quant_range_info_map[i];

	return NULL;
}
