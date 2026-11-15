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

#include "color-representation-common.h"

#include "pixel-formats.h"
#include "shared/weston-drm-fourcc.h"
#include "weston-test-assert.h"
#include "xdg-client-helper.h"

static const struct setup_args my_setup_args[] = {
	{
		.meta.name = "GL - shm",
		.renderer = WESTON_RENDERER_GL,
		.buffer_type = CLIENT_BUFFER_TYPE_SHM,
	},
	{
		.meta.name = "GL - dmabuf renderer",
		.renderer = WESTON_RENDERER_GL,
		.buffer_type = CLIENT_BUFFER_TYPE_DMABUF,
	},
	{
		.meta.name = "GL - dmabuf renderer + force-import-yuv-fallback",
		.renderer = WESTON_RENDERER_GL,
		.buffer_type = CLIENT_BUFFER_TYPE_DMABUF,
		.gl_force_import_yuv_fallback = true,
	},
	{
		.meta.name = "Vulkan - shm",
		.renderer = WESTON_RENDERER_VULKAN,
		.buffer_type = CLIENT_BUFFER_TYPE_SHM,
	},
	{
		.meta.name = "Vulkan - dmabuf renderer",
		.renderer = WESTON_RENDERER_VULKAN,
		.buffer_type = CLIENT_BUFFER_TYPE_DMABUF,
	},
};

static enum test_result_code
fixture_setup(struct weston_test_harness *harness, const struct setup_args *arg)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.renderer = arg->renderer;
	setup.refresh = HIGHEST_OUTPUT_REFRESH;
	setup.logging_scopes = "log";

	/* Required for test that also run on DRM */
	setup.width = 1024;
	setup.height = 768;

	setup.test_quirks.required_capabilities = WESTON_CAP_COLOR_REP;
	setup.test_quirks.gl_force_import_yuv_fallback =
		arg->gl_force_import_yuv_fallback;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, my_setup_args, meta);

static enum test_result_code
color_representation_renderer(struct wet_testsuite_data *suite_data,
			      const struct color_state *color_state)
{
	const struct setup_args *args = &my_setup_args[get_test_fixture_index()];

	return test_color_representation(color_state, args->buffer_type,
					 CLIENT_BUFFER_TYPE_SHM,
					 WESTON_CAPTURE_V1_SOURCE_FRAMEBUFFER,
					 FB_PRESENTED);
}

static struct client_buffer*
create_and_fill_nv12_buffer(struct client *client,
			    enum client_buffer_type buffer_type, int width,
			    int height)
{
	const struct pixel_format_info *fmt_info;
	struct client_buffer *buffer;
	uint8_t *y_base;
	uint16_t *uv_base;

	fmt_info = pixel_format_get_info(DRM_FORMAT_NV12);

	switch (buffer_type) {
	case CLIENT_BUFFER_TYPE_SHM:
		buffer = client_buffer_util_create_shm_buffer(client->wl_shm,
			fmt_info,
			width,
			height);
		break;
	case CLIENT_BUFFER_TYPE_DMABUF:
		buffer = client_buffer_util_create_dmabuf_buffer(client->wl_display,
			client->dmabuf, NULL,
			fmt_info,
			width,
			height);
		break;
	default:
		test_assert_not_reached("Buffer type not handled");
		break;
	}

	y_base = buffer->data + buffer->offsets[0];
	uv_base = (uint16_t *)(buffer->data + buffer->offsets[1]);

	client_buffer_util_maybe_sync_dmabuf_start(buffer);
	for (int y = 0; y < height; y++) {
		uint8_t *y_row;
		uint16_t *uv_row;

		y_row = y_base + y * buffer->strides[0];
		uv_row = uv_base + (y / 2) * (buffer->strides[1] / sizeof(uint16_t));

		for (int x = 0; x < width; x++) {
			*(y_row + x) = 0x30;
			*(uv_row + x / 2) = 0x5050;
		}
	}
	client_buffer_util_maybe_sync_dmabuf_end(buffer);

	return buffer;
}

static void
buffer_release(void *data, struct wl_buffer *buffer)
{
	wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
	buffer_release
};

/*
 * Test that the same NV12 buffer can be attached to multiple wl_surfaces with
 * different color representation values.
 */
static enum test_result_code
drm_color_representation_reuse_buffer(struct wet_testsuite_data *suite_data)
{
	const struct setup_args *args = &my_setup_args[get_test_fixture_index()];
	struct xdg_client *xdg_client;
	struct client *client;
	struct xdg_surface_data *xdg_surface;
	struct wl_surface *toplevel_surface;
	int n_color_state_cases = ARRAY_LENGTH(color_state_cases);
	struct wl_surface *surface[n_color_state_cases];
	struct wl_subsurface *subsurface[n_color_state_cases];
	struct wl_buffer *toplevel_buffer;
	struct client_buffer *buffer;
	struct wp_viewport *toplevel_viewport;
	struct wp_color_representation_surface_v1 *color_representation_surface[n_color_state_cases];
	struct rectangle clip = { .width = 128, .height = 128 };
	bool match;

	if (args->buffer_type == CLIENT_BUFFER_TYPE_DMABUF &&
	    !client_buffer_util_is_dmabuf_supported()) {
		testlog("%s: Skipped: udmabuf not supported\n", get_test_name());
		return RESULT_SKIP;
	}

	xdg_client = create_xdg_client();
	client = xdg_client->client;
	xdg_surface = create_xdg_surface(xdg_client);
	toplevel_surface = xdg_surface->surface->wl_surface;

	xdg_surface_make_toplevel(xdg_surface,
		"weston.test.color-representation", "one");
	xdg_toplevel_set_fullscreen(xdg_surface->xdg_toplevel, NULL);
	xdg_surface_wait_configure(xdg_surface);

	toplevel_viewport = wp_viewporter_get_viewport(client->viewporter,
						       toplevel_surface);
	wp_viewport_set_destination(toplevel_viewport,
		xdg_surface->configure.width, xdg_surface->configure.height);
	toplevel_buffer =
		wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(client->single_pixel_manager,
									 0x0, 0x0, 0x0, 0x0);
	wl_surface_attach(toplevel_surface, toplevel_buffer, 0, 0);
	wl_buffer_add_listener(toplevel_buffer, &buffer_listener, NULL);
	wl_surface_damage_buffer(toplevel_surface, 0, 0, 1, 1);

	buffer = create_and_fill_nv12_buffer(client, args->buffer_type,
		clip.width / 4, clip.height / 2 - 4);

	for (int i = 0; i < n_color_state_cases; i++) {
		surface[i] = wl_compositor_create_surface(client->wl_compositor);
		subsurface[i] =
			wl_subcompositor_get_subsurface(client->wl_subcompositor,
				surface[i], toplevel_surface);
	}

	wl_subsurface_set_position(subsurface[0], 0, 4);
	wl_subsurface_set_position(subsurface[1], 0, clip.height / 2);
	wl_subsurface_set_position(subsurface[2], clip.width / 4, 4);
	wl_subsurface_set_position(subsurface[3], clip.width / 4,
		clip.height / 2);
	wl_subsurface_set_position(subsurface[4], clip.width / 4 * 2, 4);
	wl_subsurface_set_position(subsurface[5], clip.width / 4 * 2,
		clip.height / 2);
	wl_subsurface_set_position(subsurface[6], clip.width / 4 * 3, 4);
	wl_subsurface_set_position(subsurface[7], clip.width / 4 * 3,
		clip.height / 2);

	for (int i = 0; i < n_color_state_cases; i++) {
		if (color_state_cases[i].create_color_representation_surface) {
			color_representation_surface[i] =
				wp_color_representation_manager_v1_get_surface(
					client->color_representation,
					surface[i]);
			if (color_state_cases[i].coefficients != 0)
				wp_color_representation_surface_v1_set_coefficients_and_range(
					color_representation_surface[i],
					color_state_cases[i].coefficients,
					color_state_cases[i].range);
		} else {
			color_representation_surface[i] = NULL;
		}

		wl_surface_attach(surface[i], buffer->wl_buffer, 0, 0);
		wl_surface_damage(surface[i], 0, 0, INT32_MAX, INT32_MAX);
		wl_surface_commit(surface[i]);
	}

	xdg_surface_maybe_ack_configure(xdg_surface);
	wl_surface_commit(toplevel_surface);

	struct verify_image_target vit = {
		.ref.basename = "color-representation",
		.ref.seq_no = 1,
		.clip = &clip,
	};
	match = verify_screen_content(client, &vit, 0);

	for (int i = 0; i < n_color_state_cases; i++) {
		if (color_representation_surface[i])
			wp_color_representation_surface_v1_destroy(color_representation_surface[i]);
		wl_subsurface_destroy(subsurface[i]);
		wl_surface_destroy(surface[i]);
	}
	wp_viewport_destroy(toplevel_viewport);
	client_buffer_util_destroy_buffer(buffer);
	destroy_xdg_surface(xdg_surface);
	xdg_client_destroy(xdg_client);

	test_assert_true(match);

	return RESULT_OK;
}

/*
 * Test that a sub-surface whose dmabuf wl_buffer has already been destroyed
 * by the client can still be repainted after its color representation
 * changes. Moving the sub-surface forces it to be repainted, and the GL
 * renderer to re-import the dmabuf with the new color representation.
 */
static enum test_result_code
color_representation_destroyed_buffer(struct wet_testsuite_data *suite_data)
{
	const struct setup_args *args = &my_setup_args[get_test_fixture_index()];
	struct xdg_client *xdg_client;
	struct client *client;
	struct xdg_surface_data *xdg_surface;
	struct wl_surface *toplevel_surface;
	struct wl_surface *surface;
	struct wl_subsurface *subsurface;
	struct wp_viewport *toplevel_viewport;
	struct wl_buffer *toplevel_buffer;
	struct client_buffer *buffer;
	struct wp_color_representation_surface_v1 *color_representation_surface;
	int done;

	/* The bug is in the dmabuf import path, nothing to test with shm. */
	if (args->buffer_type != CLIENT_BUFFER_TYPE_DMABUF)
		return RESULT_OK;

	if (!client_buffer_util_is_dmabuf_supported()) {
		testlog("%s: Skipped: udmabuf not supported\n", get_test_name());
		return RESULT_SKIP;
	}

	xdg_client = create_xdg_client();
	client = xdg_client->client;
	xdg_surface = create_xdg_surface(xdg_client);
	toplevel_surface = xdg_surface->surface->wl_surface;

	xdg_surface_make_toplevel(xdg_surface,
		"weston.test.color-representation", "one");
	xdg_toplevel_set_fullscreen(xdg_surface->xdg_toplevel, NULL);
	xdg_surface_wait_configure(xdg_surface);

	toplevel_viewport = wp_viewporter_get_viewport(client->viewporter,
						       toplevel_surface);
	wp_viewport_set_destination(toplevel_viewport,
		xdg_surface->configure.width, xdg_surface->configure.height);
	toplevel_buffer =
		wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(client->single_pixel_manager,
									 0x0, 0x0, 0x0, 0xffffffff);
	wl_buffer_add_listener(toplevel_buffer, &buffer_listener, NULL);
	wl_surface_attach(toplevel_surface, toplevel_buffer, 0, 0);
	wl_surface_damage_buffer(toplevel_surface, 0, 0, 1, 1);

	surface = wl_compositor_create_surface(client->wl_compositor);
	subsurface = wl_subcompositor_get_subsurface(client->wl_subcompositor,
						     surface, toplevel_surface);
	color_representation_surface =
		wp_color_representation_manager_v1_get_surface(
			client->color_representation, surface);
	wp_color_representation_surface_v1_set_coefficients_and_range(
		color_representation_surface,
		WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);

	buffer = create_and_fill_nv12_buffer(client, args->buffer_type, 64, 64);
	wl_surface_attach(surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface);

	xdg_surface_maybe_ack_configure(xdg_surface);
	frame_callback_set(toplevel_surface, &done);
	wl_surface_commit(toplevel_surface);
	frame_callback_wait(client, &done);

	client_buffer_util_destroy_buffer(buffer);

	wp_color_representation_surface_v1_set_coefficients_and_range(
		color_representation_surface,
		WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED);
	wl_surface_commit(surface);
	wl_subsurface_set_position(subsurface, 64, 0);

	toplevel_buffer =
		wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(client->single_pixel_manager,
									 0x0, 0x0, 0x0, 0xffffffff);
	wl_buffer_add_listener(toplevel_buffer, &buffer_listener, NULL);
	wl_surface_attach(toplevel_surface, toplevel_buffer, 0, 0);
	wl_surface_damage_buffer(toplevel_surface, 0, 0, 1, 1);
	frame_callback_set(toplevel_surface, &done);
	wl_surface_commit(toplevel_surface);
	frame_callback_wait(client, &done);

	wp_color_representation_surface_v1_destroy(color_representation_surface);
	wl_subsurface_destroy(subsurface);
	wl_surface_destroy(surface);
	wp_viewport_destroy(toplevel_viewport);
	destroy_xdg_surface(xdg_surface);
	xdg_client_destroy(xdg_client);

	return RESULT_OK;
}

DECLARE_TEST_LIST(
	TESTFN_ARG(color_representation_renderer, color_state_cases),
	TESTFN(drm_color_representation_reuse_buffer),
	TESTFN(color_representation_destroyed_buffer),
);
