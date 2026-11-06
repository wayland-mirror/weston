/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2015 Samsung Electronics Co., Ltd
 * Copyright 2016, 2017 Collabora, Ltd.
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

#include <semaphore.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include "tests/test-config.h"
#include "pixel-formats.h"
#include "shared/weston-drm-fourcc.h"
#include "shared/os-compatibility.h"
#include "shared/string-helpers.h"
#include "shared/xalloc.h"
#include <libweston/zalloc.h>
#include "weston-test-client-helper.h"
#include "weston-test-assert.h"
#include "image-iter.h"
#include "color-manager-client.h"
#include "weston-output-capture-client-protocol.h"
#include "presentation-time-client-protocol.h"

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) > (b)) ? (b) : (a))
#define clip(x, a, b)  min(max(x, a), b)

struct drm_format {
	uint32_t format;
	uint64_t modifier;
};

struct coefficients_and_range {
	enum wp_color_representation_surface_v1_coefficients coefficients;
	enum wp_color_representation_surface_v1_range range;
};

int
surface_contains(struct surface *surface, int x, int y)
{
	/* test whether a global x,y point is contained in the surface */
	int sx = surface->x;
	int sy = surface->y;
	int sw = surface->width;
	int sh = surface->height;
	return x >= sx && y >= sy && x < sx + sw && y < sy + sh;
}

static void
frame_callback_handler(void *data, struct wl_callback *callback, uint32_t time)
{
	int *done = data;

	*done = 1;

	wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
	frame_callback_handler
};

struct wl_callback *
frame_callback_set(struct wl_surface *surface, int *done)
{
	struct wl_callback *callback;

	*done = 0;
	callback = wl_surface_frame(surface);
	wl_callback_add_listener(callback, &frame_listener, done);

	return callback;
}

int
frame_callback_wait_nofail(struct client *client, int *done)
{
	while (!*done) {
		if (wl_display_dispatch(client->wl_display) < 0)
			return 0;
	}

	return 1;
}

static void
move_client_internal(struct client *client, int x, int y)
{
	struct surface *surface = client->surface;

	client->surface->x = x;
	client->surface->y = y;
	weston_test_move_surface(client->test->weston_test, surface->wl_surface,
			     surface->x, surface->y);
	/* The attach here is necessary because commit() will call configure
	 * only on surfaces newly attached, and the one that sets the surface
	 * position is the configure. */
	wl_surface_attach(surface->wl_surface, surface->buffer->proxy, 0, 0);
	wl_surface_damage(surface->wl_surface, 0, 0, surface->width,
			  surface->height);

}

void
move_client_frame_sync(struct client *client, int x, int y)
{
	struct surface *surface = client->surface;
	int done;

	move_client_internal(client, x, y);
	frame_callback_set(surface->wl_surface, &done);
	wl_surface_commit(surface->wl_surface);
	frame_callback_wait(client, &done);
}

void
move_client(struct client *client, int x, int y)
{
	struct surface *surface = client->surface;

	move_client_internal(client, x, y);
	wl_surface_commit(surface->wl_surface);
}

void
move_client_offscreenable(struct client *client, int x, int y)
{
	struct surface *surface = client->surface;

	move_client_internal(client, x, y);
	wl_surface_commit(surface->wl_surface);
	wl_display_roundtrip(client->wl_display);
}

static void
pointer_handle_enter(void *data, struct wl_pointer *wl_pointer,
		     uint32_t serial, struct wl_surface *wl_surface,
		     wl_fixed_t x, wl_fixed_t y)
{
	struct pointer *pointer = data;

	if (wl_surface)
		pointer->focus = wl_surface_get_user_data(wl_surface);
	else
		pointer->focus = NULL;

	pointer->serial = serial;
	pointer->x = wl_fixed_to_int(x);
	pointer->y = wl_fixed_to_int(y);

	testlog("test-client: got pointer enter %d %d, surface %p\n",
		pointer->x, pointer->y, pointer->focus);
}

static void
pointer_handle_leave(void *data, struct wl_pointer *wl_pointer,
		     uint32_t serial, struct wl_surface *wl_surface)
{
	struct pointer *pointer = data;

	pointer->serial = serial;
	pointer->focus = NULL;

	testlog("test-client: got pointer leave, surface %p\n",
		wl_surface ? wl_surface_get_user_data(wl_surface) : NULL);
}

static void
pointer_handle_motion(void *data, struct wl_pointer *wl_pointer,
		      uint32_t time_msec, wl_fixed_t x, wl_fixed_t y)
{
	struct pointer *pointer = data;

	pointer->x = wl_fixed_to_int(x);
	pointer->y = wl_fixed_to_int(y);
	pointer->motion_time_msec = time_msec;
	pointer->motion_time_timespec = pointer->input_timestamp;
	pointer->input_timestamp = (struct timespec) { 0 };

	testlog("test-client: got pointer motion %d %d\n",
		pointer->x, pointer->y);
}

static void
pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
		      uint32_t serial, uint32_t time_msec, uint32_t button,
		      uint32_t state)
{
	struct pointer *pointer = data;

	pointer->serial = serial;
	pointer->button = button;
	pointer->state = state;
	pointer->button_time_msec = time_msec;
	pointer->button_time_timespec = pointer->input_timestamp;
	pointer->input_timestamp = (struct timespec) { 0 };

	testlog("test-client: got pointer button %u %u\n", button, state);
}

static void
pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
		    uint32_t time_msec, uint32_t axis, wl_fixed_t value)
{
	struct pointer *pointer = data;

	pointer->axis = axis;
	pointer->axis_value = wl_fixed_to_double(value);
	pointer->axis_time_msec = time_msec;
	pointer->axis_time_timespec = pointer->input_timestamp;
	pointer->input_timestamp = (struct timespec) { 0 };

	testlog("test-client: got pointer axis %u %f\n",
		axis, wl_fixed_to_double(value));
}

static void
pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
	testlog("test-client: got pointer frame\n");
}

static void
pointer_handle_axis_source(void *data, struct wl_pointer *wl_pointer,
			     uint32_t source)
{
	testlog("test-client: got pointer axis source %u\n", source);
}

static void
pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
			 uint32_t time_msec, uint32_t axis)
{
	struct pointer *pointer = data;

	pointer->axis = axis;
	pointer->axis_stop_time_msec = time_msec;
	pointer->axis_stop_time_timespec = pointer->input_timestamp;
	pointer->input_timestamp = (struct timespec) { 0 };

	testlog("test-client: got pointer axis stop %u\n", axis);
}

static void
pointer_handle_axis_discrete(void *data, struct wl_pointer *wl_pointer,
			     uint32_t axis, int32_t value)
{
	testlog("test-client: got pointer axis discrete %u %d\n", axis, value);
}

static const struct wl_pointer_listener pointer_listener = {
	pointer_handle_enter,
	pointer_handle_leave,
	pointer_handle_motion,
	pointer_handle_button,
	pointer_handle_axis,
	pointer_handle_frame,
	pointer_handle_axis_source,
	pointer_handle_axis_stop,
	pointer_handle_axis_discrete,
};

static void
keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
		       uint32_t format, int fd, uint32_t size)
{
	close(fd);

	testlog("test-client: got keyboard keymap\n");
}

static void
keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
		      uint32_t serial, struct wl_surface *wl_surface,
		      struct wl_array *keys)
{
	struct keyboard *keyboard = data;

	if (wl_surface)
		keyboard->focus = wl_surface_get_user_data(wl_surface);
	else
		keyboard->focus = NULL;

	testlog("test-client: got keyboard enter, surface %p\n",
		keyboard->focus);
}

static void
keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
		      uint32_t serial, struct wl_surface *wl_surface)
{
	struct keyboard *keyboard = data;

	keyboard->focus = NULL;

	testlog("test-client: got keyboard leave, surface %p\n",
		wl_surface ? wl_surface_get_user_data(wl_surface) : NULL);
}

static void
keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
		    uint32_t serial, uint32_t time_msec, uint32_t key,
		    uint32_t state)
{
	struct keyboard *keyboard = data;

	keyboard->key = key;
	keyboard->state = state;
	keyboard->key_time_msec = time_msec;
	keyboard->key_time_timespec = keyboard->input_timestamp;
	keyboard->input_timestamp = (struct timespec) { 0 };

	testlog("test-client: got keyboard key %u %u\n", key, state);
}

static void
keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
			  uint32_t serial, uint32_t mods_depressed,
			  uint32_t mods_latched, uint32_t mods_locked,
			  uint32_t group)
{
	struct keyboard *keyboard = data;

	keyboard->mods_depressed = mods_depressed;
	keyboard->mods_latched = mods_latched;
	keyboard->mods_locked = mods_locked;
	keyboard->group = group;

	testlog("test-client: got keyboard modifiers %u %u %u %u\n",
		mods_depressed, mods_latched, mods_locked, group);
}

static void
keyboard_handle_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
			    int32_t rate, int32_t delay)
{
	struct keyboard *keyboard = data;

	keyboard->repeat_info.rate = rate;
	keyboard->repeat_info.delay = delay;

	testlog("test-client: got keyboard repeat_info %d %d\n", rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
	keyboard_handle_keymap,
	keyboard_handle_enter,
	keyboard_handle_leave,
	keyboard_handle_key,
	keyboard_handle_modifiers,
	keyboard_handle_repeat_info,
};

static void
touch_handle_down(void *data, struct wl_touch *wl_touch,
		  uint32_t serial, uint32_t time_msec,
		  struct wl_surface *surface, int32_t id,
		  wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct touch *touch = data;

	touch->down_x = wl_fixed_to_int(x_w);
	touch->down_y = wl_fixed_to_int(y_w);
	touch->id = id;
	touch->down_time_msec = time_msec;
	touch->down_time_timespec = touch->input_timestamp;
	touch->input_timestamp = (struct timespec) { 0 };

	testlog("test-client: got touch down %d %d, surf: %p, id: %d\n",
		touch->down_x, touch->down_y, surface, id);
}

static void
touch_handle_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time_msec, int32_t id)
{
	struct touch *touch = data;
	touch->up_id = id;
	touch->up_time_msec = time_msec;
	touch->up_time_timespec = touch->input_timestamp;
	touch->input_timestamp = (struct timespec) { 0 };

	testlog("test-client: got touch up, id: %d\n", id);
}

static void
touch_handle_motion(void *data, struct wl_touch *wl_touch,
		    uint32_t time_msec, int32_t id,
		    wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct touch *touch = data;
	touch->x = wl_fixed_to_int(x_w);
	touch->y = wl_fixed_to_int(y_w);
	touch->motion_time_msec = time_msec;
	touch->motion_time_timespec = touch->input_timestamp;
	touch->input_timestamp = (struct timespec) { 0 };

	testlog("test-client: got touch motion, %d %d, id: %d\n",
		touch->x, touch->y, id);
}

static void
touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
	struct touch *touch = data;

	++touch->frame_no;

	testlog("test-client: got touch frame (%d)\n", touch->frame_no);
}

static void
touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
	struct touch *touch = data;

	++touch->cancel_no;

	testlog("test-client: got touch cancel (%d)\n", touch->cancel_no);
}

static const struct wl_touch_listener touch_listener = {
	touch_handle_down,
	touch_handle_up,
	touch_handle_motion,
	touch_handle_frame,
	touch_handle_cancel,
};

static void
surface_enter(void *data,
	      struct wl_surface *wl_surface, struct wl_output *output)
{
	struct surface *surface = data;

	surface->output = wl_output_get_user_data(output);

	testlog("test-client: got surface enter output %p\n", surface->output);
}

static void
surface_leave(void *data,
	      struct wl_surface *wl_surface, struct wl_output *output)
{
	struct surface *surface = data;

	surface->output = NULL;

	testlog("test-client: got surface leave output %p\n",
		wl_output_get_user_data(output));
}

#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
static void
surface_preferred_buffer_scale(void *data,
			       struct wl_surface *wl_surface,
			       int32_t scale)
{
	struct surface *surface = data;

	surface->preferred_buffer_scale = scale;
}
#endif

#ifdef WL_SURFACE_PREFERRED_BUFFER_TRANSFORM_SINCE_VERSION
static void
surface_preferred_buffer_transform(void *data,
				   struct wl_surface *wl_surface,
				   uint32_t transform)
{
	struct surface *surface = data;

	surface->preferred_buffer_transform = transform;
}
#endif

static const struct wl_surface_listener surface_listener = {
	surface_enter,
	surface_leave,
#ifdef WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
	surface_preferred_buffer_scale,
#endif
#ifdef WL_SURFACE_PREFERRED_BUFFER_TRANSFORM_SINCE_VERSION
	surface_preferred_buffer_transform,
#endif
};

bool
support_shm_format(struct client *client, uint32_t shm_format)
{
	uint32_t *p;

	wl_array_for_each(p, &client->shm_formats)
		if (*p == shm_format)
			return true;

	return false;
}

struct buffer *
create_buffer(struct client *client, int width, int height, uint32_t drm_format,
	      enum client_buffer_type buffer_type)
{
	const struct pixel_format_info *pfmt;
	struct buffer *buf;

	test_assert_int_gt(width, 0);
	test_assert_int_gt(height, 0);

	pfmt = pixel_format_get_info(drm_format);
	test_assert_ptr_not_null(pfmt);

	buf = xzalloc(sizeof *buf);

	if (buffer_type == CLIENT_BUFFER_TYPE_SHM) {
		uint32_t shm_format;

		shm_format = pixel_format_get_shm_format(pfmt);

		if (!support_shm_format(client, shm_format))
		    return NULL;

		buf->buf = client_buffer_util_create_shm_buffer(client->wl_shm,
								pfmt,
								width,
								height);
	} else {
		test_assert_true(buffer_type == CLIENT_BUFFER_TYPE_DMABUF);

		if (!support_drm_format(client, drm_format, DRM_FORMAT_MOD_LINEAR))
		    return NULL;

		buf->buf = client_buffer_util_create_dmabuf_buffer(client->wl_display,
								   client->dmabuf, NULL,
								   pfmt,
								   width,
								   height);
	}
	test_assert_ptr_not_null(buf->buf);
	buf->proxy = buf->buf->wl_buffer;

	buf->image = pixman_image_create_bits(pfmt->pixman_format,
					      width, height,
					      buf->buf->data,
					      buf->buf->strides[0]);

	test_assert_ptr_not_null(buf->proxy);
	test_assert_ptr_not_null(buf->image);

	return buf;
}

struct buffer *
create_shm_buffer(struct client *client, int width, int height,
		  uint32_t drm_format)
{
	return create_buffer(client, width, height, drm_format,
			     CLIENT_BUFFER_TYPE_SHM);
}

struct buffer *
create_shm_buffer_a8r8g8b8(struct client *client, int width, int height)
{
	const struct pixel_format_info *pfmt;

	/* DRM formats are little-endian, PIXMAN_a8r8g8b8 is native-endian, so
	 * the DRM format to ask for depends on the host. */
	pfmt = pixel_format_get_info_by_pixman(PIXMAN_a8r8g8b8);
	test_assert_ptr_not_null(pfmt);

	return create_shm_buffer(client, width, height, pfmt->format);
}

static struct buffer *
create_pixman_buffer(int width, int height, pixman_format_code_t pixman_format)
{
	struct buffer *buf;

	test_assert_int_gt(width, 0);
	test_assert_int_gt(height, 0);

	buf = xzalloc(sizeof *buf);
	buf->image = pixman_image_create_bits(pixman_format,
					      width, height, NULL, 0);
	test_assert_ptr_not_null(buf->image);

	return buf;
}

void
buffer_destroy(struct buffer *buf)
{
	test_assert_true(pixman_image_unref(buf->image));

	if (buf->buf)
		client_buffer_util_destroy_buffer(buf->buf);
	free(buf);
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
	struct client *client = data;
	uint32_t *p;

	p = wl_array_add(&client->shm_formats, sizeof *p);
	assert(p);
	*p = format;
}

struct wl_shm_listener shm_listener = {
	shm_format
};

bool
support_drm_format(struct client *client, uint32_t format, uint64_t modifier)
{
	struct drm_format *p;

	wl_array_for_each(p, &client->drm_formats)
		if (p->format == format && p->modifier == modifier)
			return true;

	return false;
}

static void
dmabuf_modifier(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
		 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
	struct client *client = data;
	uint64_t modifier = u64_from_u32s(modifier_hi, modifier_lo);
	struct drm_format *p;

	p = wl_array_add(&client->drm_formats, sizeof *p);
	assert(p);
	p->format = format;
	p->modifier = modifier;
}


static void
dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
              uint32_t format)
{
	/* deprecated */
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
	dmabuf_format,
	dmabuf_modifier
};

bool
support_coefficients_and_range(struct client *client,
			       enum wp_color_representation_surface_v1_coefficients coefficients,
			       enum wp_color_representation_surface_v1_range range)
{
	struct coefficients_and_range *p;

	wl_array_for_each(p, &client->coefficients_and_ranges)
		if (p->coefficients == coefficients && p->range == range)
			return true;

	return false;
}

static void
color_representation_supported_alpha_mode(void *data,
					  struct wp_color_representation_manager_v1 *wp_color_representation_manager_v1,
					  uint32_t alpha_mode)
{
}

static void
color_representation_supported_coefficients_and_ranges(void *data,
						       struct wp_color_representation_manager_v1 *wp_color_representation_manager_v1,
						       uint32_t coefficients,
						       uint32_t range)
{
	struct client *client = data;
	struct coefficients_and_range *p;

	p = wl_array_add(&client->coefficients_and_ranges, sizeof *p);
	assert(p);
	p->coefficients = coefficients;
	p->range = range;
}

static void
color_representation_done(void *data,
			  struct wp_color_representation_manager_v1 *wp_color_representation_manager_v1)
{
}

static const struct wp_color_representation_manager_v1_listener color_representation_listener = {
	.supported_alpha_mode = color_representation_supported_alpha_mode,
	.supported_coefficients_and_ranges = color_representation_supported_coefficients_and_ranges,
	.done = color_representation_done,
};

static void
test_handle_pointer_position(void *data, struct weston_test *weston_test,
			     wl_fixed_t x, wl_fixed_t y)
{
	struct test *test = data;
	test->pointer_x = wl_fixed_to_int(x);
	test->pointer_y = wl_fixed_to_int(y);

	testlog("test-client: got global pointer %d %d\n",
		test->pointer_x, test->pointer_y);
}

static const struct weston_test_listener test_listener = {
	test_handle_pointer_position,
};

static void
input_destroy(struct input *inp)
{
	if (inp->pointer) {
		wl_pointer_release(inp->pointer->wl_pointer);
		free(inp->pointer);
	}
	if (inp->keyboard) {
		wl_keyboard_release(inp->keyboard->wl_keyboard);
		free(inp->keyboard);
	}
	if (inp->touch) {
		wl_touch_release(inp->touch->wl_touch);
		free(inp->touch);
	}
	wl_list_remove(&inp->link);
	wl_seat_release(inp->wl_seat);
	free(inp->seat_name);
	free(inp);
}

static void
input_update_devices(struct input *input)
{
	struct pointer *pointer;
	struct keyboard *keyboard;
	struct touch *touch;

	struct wl_seat *seat = input->wl_seat;
	enum wl_seat_capability caps = input->caps;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer) {
		pointer = xzalloc(sizeof *pointer);
		pointer->wl_pointer = wl_seat_get_pointer(seat);
		wl_pointer_set_user_data(pointer->wl_pointer, pointer);
		wl_pointer_add_listener(pointer->wl_pointer, &pointer_listener,
					pointer);
		input->pointer = pointer;
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer) {
		wl_pointer_destroy(input->pointer->wl_pointer);
		free(input->pointer);
		input->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard) {
		keyboard = xzalloc(sizeof *keyboard);
		keyboard->wl_keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_set_user_data(keyboard->wl_keyboard, keyboard);
		wl_keyboard_add_listener(keyboard->wl_keyboard, &keyboard_listener,
					 keyboard);
		input->keyboard = keyboard;
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->keyboard) {
		wl_keyboard_destroy(input->keyboard->wl_keyboard);
		free(input->keyboard);
		input->keyboard = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !input->touch) {
		touch = xzalloc(sizeof *touch);
		touch->wl_touch = wl_seat_get_touch(seat);
		wl_touch_set_user_data(touch->wl_touch, touch);
		wl_touch_add_listener(touch->wl_touch, &touch_listener,
					 touch);
		input->touch = touch;
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && input->touch) {
		wl_touch_destroy(input->touch->wl_touch);
		free(input->touch);
		input->touch = NULL;
	}
}

static void
seat_handle_capabilities(void *data, struct wl_seat *seat,
			 enum wl_seat_capability caps)
{
	struct input *input = data;

	input->caps = caps;

	/* we will create/update the devices only with the right (test) seat.
	 * If we haven't discovered which seat is the test seat, just
	 * store capabilities and bail out */
	if (input->seat_name && strcmp(input->seat_name, "test-seat") == 0)
		input_update_devices(input);

	testlog("test-client: got seat %p capabilities: %x\n", input, caps);
}

static void
seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
	struct input *input = data;

	input->seat_name = strdup(name);
	test_assert_ptr_not_null(input->seat_name);

	/* We only update the devices and set client input for the test seat */
	if (strcmp(name, "test-seat") == 0) {
		/* Can't have multiple test seats. */
		test_assert_ptr_null(input->client->input);

		input_update_devices(input);
		input->client->input = input;
	}

	testlog("test-client: got seat %p name: \'%s\'\n", input, name);
}

static const struct wl_seat_listener seat_listener = {
	seat_handle_capabilities,
	seat_handle_name,
};

static void
output_handle_geometry(void *data,
		       struct wl_output *wl_output,
		       int x, int y,
		       int physical_width,
		       int physical_height,
		       int subpixel,
		       const char *make,
		       const char *model,
		       int32_t transform)
{
	struct output *output = data;

	output->x = x;
	output->y = y;
}

static void
output_handle_mode(void *data,
		   struct wl_output *wl_output,
		   uint32_t flags,
		   int width,
		   int height,
		   int refresh)
{
	struct output *output = data;

	if (flags & WL_OUTPUT_MODE_CURRENT) {
		output->width = width;
		output->height = height;
	}
}

static void
output_handle_scale(void *data,
		    struct wl_output *wl_output,
		    int scale)
{
	struct output *output = data;

	output->scale = scale;
}

static void
output_handle_name(void *data, struct wl_output *wl_output, const char *name)
{
	struct output *output = data;
	output->name = strdup(name);
}

static void
output_handle_description(void *data, struct wl_output *wl_output, const char *desc)
{
	struct output *output = data;
	output->desc = strdup(desc);
}

static void
output_handle_done(void *data,
		   struct wl_output *wl_output)
{
	struct output *output = data;

	output->initialized = 1;
}

static const struct wl_output_listener output_listener = {
	output_handle_geometry,
	output_handle_mode,
	output_handle_done,
	output_handle_scale,
	output_handle_name,
	output_handle_description,
};

static void
output_destroy(struct output *output)
{
	test_assert_u32_ge(wl_proxy_get_version((struct wl_proxy *)output->wl_output), 3);
	wl_output_release(output->wl_output);
	wl_list_remove(&output->link);
	free(output->name);
	free(output->desc);
	free(output);
}

static void
handle_global(void *data, struct wl_registry *registry,
	      uint32_t id, const char *interface, uint32_t version)
{
	struct client *client = data;
	struct output *output;
	struct test *test;
	struct global *global;
	struct input *input;

	global = xzalloc(sizeof *global);
	global->name = id;
	global->interface = strdup(interface);
	test_assert_ptr_not_null(interface);
	global->version = version;
	wl_list_insert(client->global_list.prev, &global->link);

	/* We deliberately bind all globals with the maximum (advertised)
	 * version, because this test suite must be kept up-to-date with
	 * Weston. We must always implement at least the version advertised
	 * by Weston. This is not ok for normal clients, but it is ok in
	 * this test suite.
	 */

	if (strcmp(interface, "wl_compositor") == 0) {
		client->wl_compositor =
			wl_registry_bind(registry, id,
					 &wl_compositor_interface, version);
	} else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
		client->wl_subcompositor =
			wl_registry_bind(registry, id,
					 &wl_subcompositor_interface, version);
	} else if (strcmp(interface, "wl_seat") == 0) {
		input = xzalloc(sizeof *input);
		input->client = client;
		input->global_name = global->name;
		input->wl_seat =
			wl_registry_bind(registry, id,
					 &wl_seat_interface, version);
		wl_seat_add_listener(input->wl_seat, &seat_listener, input);
		wl_list_insert(&client->inputs, &input->link);
	} else if (strcmp(interface, "wl_shm") == 0) {
		client->wl_shm =
			wl_registry_bind(registry, id,
					 &wl_shm_interface, version);
		wl_shm_add_listener(client->wl_shm, &shm_listener, client);
	} else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		client->dmabuf =
			wl_registry_bind(registry, id,
					 &zwp_linux_dmabuf_v1_interface, 3);
		zwp_linux_dmabuf_v1_add_listener(client->dmabuf,
						 &dmabuf_listener, client);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		client->viewporter = wl_registry_bind(registry, id,
						      &wp_viewporter_interface,
						      1);
	} else if (strcmp(interface, wp_presentation_interface.name) == 0) {
		client->presentation =
			wl_registry_bind(registry, id,
					 &wp_presentation_interface, 1);
	} else if (strcmp(interface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		client->single_pixel_manager =
			wl_registry_bind(registry, id,
					 &wp_single_pixel_buffer_manager_v1_interface,
					 1);
	} else if (strcmp(interface, wp_color_representation_manager_v1_interface.name) == 0) {
		client->color_representation =
			wl_registry_bind(registry, id,
					 &wp_color_representation_manager_v1_interface, version);
		wp_color_representation_manager_v1_add_listener (client->color_representation,
								 &color_representation_listener,
								 client);
	} else if (strcmp(interface, "wl_output") == 0) {
		output = xzalloc(sizeof *output);
		output->wl_output =
			wl_registry_bind(registry, id,
					 &wl_output_interface, version);
		wl_output_add_listener(output->wl_output,
				       &output_listener, output);
		wl_list_insert(&client->output_list, &output->link);
		client->output = output;
	} else if (strcmp(interface, "weston_test") == 0) {
		test = xzalloc(sizeof *test);
		test->weston_test =
			wl_registry_bind(registry, id,
					 &weston_test_interface, version);
		weston_test_add_listener(test->weston_test, &test_listener, test);
		client->test = test;
	} else if (strcmp(interface, wp_fifo_manager_v1_interface.name) == 0) {
		client->fifo_manager =
			wl_registry_bind(registry, id,
					 &wp_fifo_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_commit_timing_manager_v1_interface.name) == 0) {
		client->commit_timing_manager =
			wl_registry_bind(registry, id,
					 &wp_commit_timing_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_alpha_modifier_v1_interface.name) == 0) {
		client->alpha_modifier =
			wl_registry_bind(registry, id,
					 &wp_alpha_modifier_v1_interface, 1);
	} else if (strcmp(interface, wp_tearing_control_manager_v1_interface.name) == 0) {
		client->tearing_control_manager =
			wl_registry_bind(registry, id,
					 &wp_tearing_control_manager_v1_interface, 1);
	} else if (strcmp(interface, weston_restricted_buffer_v1_interface.name) == 0) {
		client->restricted_buffer =
			wl_registry_bind(registry, id,
					 &weston_restricted_buffer_v1_interface, 1);
	} else if (strcmp(interface, weston_fast_forward_manager_v1_interface.name) == 0) {
		client->weston_fast_forward_manager =
			wl_registry_bind(registry, id,
					 &weston_fast_forward_manager_v1_interface, 1);
	}
}

static struct global *
client_find_global_with_name(struct client *client, uint32_t name)
{
	struct global *global;

	wl_list_for_each(global, &client->global_list, link) {
		if (global->name == name)
			return global;
	}

	return NULL;
}

static struct input *
client_find_input_with_name(struct client *client, uint32_t name)
{
	struct input *input;

	wl_list_for_each(input, &client->inputs, link) {
		if (input->global_name == name)
			return input;
	}

	return NULL;
}

static void
global_destroy(struct global *global)
{
	wl_list_remove(&global->link);
	free(global->interface);
	free(global);
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	struct client *client = data;
	struct global *global;
	struct input *input;

	global = client_find_global_with_name(client, name);

	/* Unknown global. */
	test_assert_ptr_not_null(global);

	if (strcmp(global->interface, "wl_seat") == 0) {
		input = client_find_input_with_name(client, name);
		if (input) {
			if (client->input == input)
				client->input = NULL;
			input_destroy(input);
		}
	}

	/* XXX: handle wl_output */

	global_destroy(global);
}

static const struct wl_registry_listener registry_listener = {
	handle_global,
	handle_global_remove,
};

/**
 * Expect protocol error
 *
 * @param client A client instance, as created by create_client().
 * @param intf The interface where the error should occur. NULL is also valid,
 * when the error should come from an unknown object.
 * @param code The error code that is expected.
 *
 * To exercise our protocol implementations, tests can use this function when
 * they do something on purpose expecting a protocol error.
 *
 * When tests know that their wl_proxy would get destroyed before they have a
 * chance to call this, they should pass a NULL intf to expect the error from an
 * unknown object. When wl_display.error event comes, it will refer to an object
 * id that has already been marked as deleted in the client's object map, so
 * the protocol error interface will be set to NULL and the object id to 0.
 */
void
expect_protocol_error(struct client *client,
		      const struct wl_interface *intf,
		      uint32_t code)
{
	int err;
	uint32_t errcode, failed = 0;
	const struct wl_interface *interface;
	unsigned int id;

	/* if the error has not come yet, make it happen */
	wl_display_roundtrip(client->wl_display);

	err = wl_display_get_error(client->wl_display);

	/* Expected protocol error but nothing came. */
	test_assert_int_ne(err, 0);

	/* Expected protocol error but got local error. */
	test_assert_enum_eq(err, EPROTO);

	errcode = wl_display_get_protocol_error(client->wl_display,
						&interface, &id);

	/* check error */
	if (errcode != code) {
		testlog("Should get error code %d but got %d\n", code, errcode);
		failed = 1;
	}

	if (intf && !interface) {
		testlog("Should get interface '%s' but got error from unknown object\n",
			intf->name);
		failed = 1;
	} else if (!intf && interface) {
		testlog("Should get error from unknown object but got it from interface '%s'\n",
			interface->name);
		failed = 1;
	} else if (intf && interface && strcmp(intf->name, interface->name) != 0) {
		testlog("Should get interface '%s' but got '%s'\n",
			intf->name, interface->name);
		failed = 1;
	}

	if (failed) {
		testlog("Expected other protocol error\n");
		abort();
	}

	/* all OK */
	if (intf)
		testlog("Got expected protocol error on '%s' (object id: %d) " \
			"with code %d\n", intf->name, id, errcode);
	else
		testlog("Got expected protocol error on unknown object " \
			"with code %d\n", errcode);

	client->errored_ok = true;
}

static void
log_handler(const char *fmt, va_list args)
{
	fprintf(stderr, "libwayland: ");
	vfprintf(stderr, fmt, args);
}

struct client *
create_client(void)
{
	struct client *client;

	wl_log_set_handler_client(log_handler);

	/* connect to display */
	client = xzalloc(sizeof *client);
	client->wl_display = wl_display_connect(NULL);
	test_assert_ptr_not_null(client->wl_display);
	wl_array_init(&client->shm_formats);
	wl_array_init(&client->drm_formats);
	wl_array_init(&client->coefficients_and_ranges);
	wl_list_init(&client->global_list);
	wl_list_init(&client->inputs);
	wl_list_init(&client->output_list);

	/* setup registry so we can bind to interfaces */
	client->wl_registry = wl_display_get_registry(client->wl_display);
	wl_registry_add_listener(client->wl_registry, &registry_listener,
				 client);

	/* this roundtrip makes sure we have all globals and we bound to them */
	client_roundtrip(client);
	/* this roundtrip makes sure we got all wl_shm.format and wl_seat.*
	 * events */
	client_roundtrip(client);

	/* must have WL_SHM_FORMAT_*RGB8888 */
	test_assert_true(support_shm_format(client, WL_SHM_FORMAT_ARGB8888));
	test_assert_true(support_shm_format(client, WL_SHM_FORMAT_XRGB8888));

	/* must have weston_test interface */
	test_assert_ptr_not_null(client->test);

	/* must have an output */
	test_assert_ptr_not_null(client->output);

	/* the output must be initialized */
	test_assert_int_eq(client->output->initialized, 1);

	/* must have seat set */
	test_assert_ptr_not_null(client->input);

	return client;
}

struct surface *
create_test_surface(struct client *client)
{
	struct surface *surface;

	surface = xzalloc(sizeof *surface);

	surface->client = client;
	surface->preferred_buffer_transform = WL_OUTPUT_TRANSFORM_NORMAL;
	surface->preferred_buffer_scale = 1;
	surface->wl_surface =
		wl_compositor_create_surface(client->wl_compositor);
	test_assert_ptr_not_null(surface->wl_surface);

	wl_surface_add_listener(surface->wl_surface, &surface_listener,
				surface);

	wl_surface_set_user_data(surface->wl_surface, surface);

	return surface;
}

void
surface_destroy(struct surface *surface)
{
	if (surface->wl_surface)
		wl_surface_destroy(surface->wl_surface);
	if (surface->buffer)
		buffer_destroy(surface->buffer);
	free(surface);
}

void
surface_set_opaque_rect(struct surface *surface, const struct rectangle *rect)
{
	struct wl_region *region;

	region = wl_compositor_create_region(surface->client->wl_compositor);
	wl_region_add(region, rect->x, rect->y, rect->width, rect->height);
	wl_surface_set_opaque_region(surface->wl_surface, region);
	wl_region_destroy(region);
}

struct client *
create_client_and_test_surface(int x, int y, int width, int height)
{
	struct client *client;
	struct surface *surface;
	pixman_color_t color = { 16384, 16384, 16384, 16384 }; /* uint16_t */

	client = create_client();

	/* initialize the client surface */
	surface = create_test_surface(client);
	client->surface = surface;

	surface->width = width;
	surface->height = height;
	surface->buffer = create_shm_buffer_solid(client, width, height, &color);

	move_client_frame_sync(client, x, y);

	return client;
}

void
client_destroy(struct client *client)
{
	int ret;

	color_manager_client_destroy(client->color_manager);

	if (client->fifo_manager)
		wp_fifo_manager_v1_destroy(client->fifo_manager);

	if (client->commit_timing_manager)
		wp_commit_timing_manager_v1_destroy(client->commit_timing_manager);

	if (client->tearing_control_manager)
		wp_tearing_control_manager_v1_destroy(client->tearing_control_manager);

	if (client->weston_fast_forward_manager)
		weston_fast_forward_manager_v1_destroy(client->weston_fast_forward_manager);

	if (client->alpha_modifier)
		wp_alpha_modifier_v1_destroy(client->alpha_modifier);

	if (client->surface)
		surface_destroy(client->surface);

	wl_array_release(&client->shm_formats);
	wl_array_release(&client->drm_formats);
	wl_array_release(&client->coefficients_and_ranges);

	while (!wl_list_empty(&client->inputs)) {
		input_destroy(container_of(client->inputs.next,
					   struct input, link));
	}

	while (!wl_list_empty(&client->output_list)) {
		output_destroy(container_of(client->output_list.next,
					    struct output, link));
	}

	while (!wl_list_empty(&client->global_list)) {
		global_destroy(container_of(client->global_list.next,
					    struct global, link));
	}

	if (client->test) {
		weston_test_destroy(client->test->weston_test);
		free(client->test);
	}

	if (client->wl_shm)
		wl_shm_destroy(client->wl_shm);
	if (client->dmabuf)
		zwp_linux_dmabuf_v1_destroy(client->dmabuf);
	if (client->viewporter)
		wp_viewporter_destroy(client->viewporter);
	if (client->presentation)
		wp_presentation_destroy(client->presentation);
	if (client->restricted_buffer)
		weston_restricted_buffer_v1_destroy(client->restricted_buffer);
	if (client->single_pixel_manager)
		wp_single_pixel_buffer_manager_v1_destroy(client->single_pixel_manager);
	if (client->color_representation)
		wp_color_representation_manager_v1_destroy(client->color_representation);
	if (client->wl_compositor)
		wl_compositor_destroy(client->wl_compositor);
	if (client->wl_subcompositor)
		wl_subcompositor_destroy(client->wl_subcompositor);
	if (client->wl_registry)
		wl_registry_destroy(client->wl_registry);

	if (client->wl_display) {
		ret = wl_display_roundtrip(client->wl_display);
		test_assert_true(client->errored_ok || ret >= 0);
		wl_display_disconnect(client->wl_display);
	}

	free(client);
}

static const char*
output_path(void)
{
	char *path = getenv("WESTON_TEST_OUTPUT_PATH");

	if (!path)
		return ".";

	return path;
}

const char *
reference_path(void)
{
	char *path = getenv("WESTON_TEST_REFERENCE_PATH");

	if (!path)
		return WESTON_TEST_REFERENCE_PATH;
	return path;
}

char*
screenshot_reference_filename(const char *basename, uint32_t seq)
{
	char *filename;

	if (asprintf(&filename, "%s/%s-%02d.png",
				 reference_path(), basename, seq) < 0)
		return NULL;
	return filename;
}

char *
image_filename(const char *basename)
{
	char *filename;
	int ret;

	ret = asprintf(&filename, "%s/%s.png", reference_path(), basename);
	test_assert_int_ge(ret, 0);

	return filename;
}

/** Helper to create filenames for test programs.
 *
 * \param test_program The test program name.
 * \param suffix Arbitrary suffix to append after the test program name.
 * Optional, NULL is valid as well.
 * \param file_ext The file extension (without '.').
 * \return The ICC filename.
 */
char *
output_filename_for_test_program(const char *test_program, const char *suffix,
				 const char *file_ext)
{
	char *filename;

	test_assert_ptr_not_null(test_program);
	test_assert_ptr_not_null(file_ext);

	if (suffix)
		str_printf(&filename, "%s/%s-%s.%s", output_path(), test_program,
						     suffix, file_ext);
	else
		str_printf(&filename, "%s/%s.%s", output_path(), test_program,
						  file_ext);

	test_assert_ptr_not_null(filename);
	return filename;
}

/** Helper to create filenames for fixtures.
 *
 * \param test_program The test program name.
 * \param harness The test harness, from which we get the fixture number.
 * \param suffix Arbitrary suffix to append after the fixture number. Optional,
 * NULL is valid as well.
 * \param file_ext The file extension (without '.').
 * \return The ICC filename.
 */
char *
output_filename_for_fixture(const char *test_program,
			    struct weston_test_harness *harness,
			    const char *suffix, const char *file_ext)
{
	int fixture_number;
	char *filename;

	test_assert_ptr_not_null(test_program);
	test_assert_ptr_not_null(harness);
	test_assert_ptr_not_null(file_ext);

	fixture_number = get_test_fixture_number_from_harness(harness);

	if (suffix)
		str_printf(&filename, "%s/%s-f%02d-%s.%s", output_path(), test_program,
							   fixture_number, suffix, file_ext);
	else
		str_printf(&filename, "%s/%s-f%02d.%s", output_path(), test_program,
							fixture_number, file_ext);

	test_assert_ptr_not_null(filename);
	return filename;
}

/** Helper to create filenames for test cases.
 *
 * \param suffix Arbitrary suffix to append after the test case name. Optional,
 * NULL is valid as well.
 * \param seq_number To differentiate filenames created from a loop. Simply use
 * 0 if not in a loop.
 * \param file_ext The file extension (without '.').
 * \return The ICC filename.
 *
 * This is only usable from code paths invoked through \c DECLARE_TEST_LIST() .
 */
char *
output_filename_for_test_case(const char *suffix, uint32_t seq_number,
			      const char *file_ext)
{
	char *filename;

	test_assert_ptr_not_null(file_ext);

	if (suffix)
		str_printf(&filename, "%s/%s-%s-%02d.%s", output_path(), get_test_name(),
							  suffix, seq_number, file_ext);
	else
		str_printf(&filename, "%s/%s-%02d.%s", output_path(), get_test_name(),
						       seq_number, file_ext);

	test_assert_ptr_not_null(filename);
	return filename;
}

/** Open a writable file
 *
 * \param suffix Custom file name suffix.
 * \return FILE pointer, or NULL on failure.
 *
 * The file name consists of output path, test name, and the given suffix.
 * If environment variable WESTON_TEST_OUTPUT_PATH is set, it is used as the
 * directory path, otherwise the current directory is used.
 *
 * The file will be writable. If it exists, it is truncated, otherwise it is
 * created. Failures are logged.
 */
FILE *
fopen_dump_file(const char *suffix)
{
	char *fname;
	FILE *fp;

	str_printf(&fname, "%s/%s-%s.txt", output_path(),
		   get_test_name(), suffix);
	fp = fopen(fname, "w");
	if (!fp) {
		testlog("Error: failed to open file '%s' for writing: %s\n",
			fname, strerror(errno));
	}
	free(fname);

	return fp;
}

/**
 * Read a file into a newly malloc'd memory.
 *
 * \param fname Full name of the file to read.
 * \param data_out Pointer where to store the pointer to the read data.
 * \return Length of the data, or 0 on error.
 *
 * On error, *data_out is not modified. On success, *data_out contains a
 * pointer to the data and must be free()'d by the caller.
 */
size_t
read_blob_from_file(const char *fname, char **data_out)
{
	struct wl_array data;
	char tmpbuf[64];
	FILE *fp;
	size_t len;
	void *p;
	int fer;

	testlog("%s: %s\n", __func__, fname);

	wl_array_init(&data);

	fp = fopen(fname, "rb");
	if (!test_assert_ptr_not_null(fp)) {
		wl_array_release(&data);
		return 0;
	}

	while (!feof(fp)) {
		len = fread(tmpbuf, 1, sizeof tmpbuf, fp);
		if (len == 0)
			break;

		p = wl_array_add(&data, len);
		abort_oom_if_null(p);
		memcpy(p, tmpbuf, len);
	}

	fer = ferror(fp);
	fclose(fp);

	if (fer) {
		wl_array_release(&data);
		return 0;
	}

	*data_out = data.data;
	return data.size;
}

struct output_capturer {
	int width;
	int height;
	uint32_t drm_format;
	bool formats_done;

	struct weston_capture_v1 *factory;
	struct weston_capture_source_v1 *source;

	bool complete;
};

static void
output_capturer_handle_format(void *data,
			      struct weston_capture_source_v1 *proxy,
			      uint32_t drm_format)
{
	struct output_capturer *capt = data;

	if (capt->formats_done) {
		capt->drm_format = DRM_FORMAT_INVALID;
		capt->formats_done = false;
	}

	if (!capt->drm_format)
		capt->drm_format = drm_format;
}

static void
output_capturer_handle_formats_done(void *data,
				    struct weston_capture_source_v1 *proxy)
{
	struct output_capturer *capt = data;

	capt->formats_done = true;
}

static void
output_capturer_handle_size(void *data,
			    struct weston_capture_source_v1 *proxy,
			    int32_t width, int32_t height)
{
	struct output_capturer *capt = data;

	capt->width = width;
	capt->height = height;
}

static void
output_capturer_handle_complete(void *data,
			        struct weston_capture_source_v1 *proxy)
{
	struct output_capturer *capt = data;

	capt->complete = true;
}

static void
output_capturer_handle_retry(void *data,
			     struct weston_capture_source_v1 *proxy)
{
	test_assert_not_reached("output capture retry in tests indicates a race");
}

static void
output_capturer_handle_failed(void *data,
			      struct weston_capture_source_v1 *proxy,
			      const char *msg)
{
	testlog("output capture failed: %s", msg ? msg : "?");

	test_assert_not_reached("output capture failed");
}

static const struct weston_capture_source_v1_listener output_capturer_source_handlers = {
	.format = output_capturer_handle_format,
	.formats_done = output_capturer_handle_formats_done,
	.size = output_capturer_handle_size,
	.complete = output_capturer_handle_complete,
	.retry = output_capturer_handle_retry,
	.failed = output_capturer_handle_failed,
};

struct buffer *
client_capture_output(struct client *client,
		      struct output *output,
		      enum weston_capture_v1_source src,
		      enum client_buffer_type buffer_type)
{
	struct output_capturer capt = {};
	struct buffer *buf;

	capt.factory = bind_to_singleton_global(client,
						&weston_capture_v1_interface,
						2);

	capt.source = weston_capture_v1_create(capt.factory,
					       output->wl_output, src);
	weston_capture_source_v1_add_listener(capt.source,
					      &output_capturer_source_handlers,
					      &capt);

	client_roundtrip(client);

	test_assert_true(capt.width != 0 &&
			 capt.height != 0 &&
			 capt.drm_format != 0 &&
			 capt.formats_done &&
			 "capture source not available");

	buf = create_buffer(client, capt.width, capt.height, capt.drm_format,
			    buffer_type);
	if (!test_assert_ptr_not_null(buf))
		abort();

	weston_capture_source_v1_capture(capt.source, buf->proxy);
	while (!capt.complete)
		if (!test_assert_int_ge(wl_display_dispatch(client->wl_display), 0))
			break;

	weston_capture_source_v1_destroy(capt.source);
	weston_capture_v1_destroy(capt.factory);

	return buf;
}

/**
 * Take screenshot of a single output
 *
 * Requests a screenshot from the server of the output specified
 * in output_name. This implies that the compositor goes through an output
 * repaint to provide the screenshot before this function returns. This
 * function is therefore both a server roundtrip and a wait for a repaint.
 *
 * The resulting buffer shall contain a copy of the framebuffer contents,
 * the output area only, that is, without borders (output decorations).
 * The shot is in output physical pixels, with the output scale and
 * orientation rather than scale=1 or orientation=normal. The pixel format
 * is ensured to be PIXMAN_a8r8g8b8.
 *
 * @param client a client instance, as created by create_client()
 * @param output_name the name of the output, as specified by wl_output.name,
 * or NULL to use the client-defined output
 * @param include_decorations true if the screenshot should include output
 * decorations, or false if it should include just the client content
 * @returns A new buffer object, that should be freed with buffer_destroy().
 */
struct buffer *
capture_screenshot_of_output(struct client *client, const char *output_name,
			     enum screenshot_decoration_mode include_decorations)
{
	struct image_header ih;
	struct buffer *shm;
	struct buffer *buf;
	struct output *output = NULL;
	enum weston_capture_v1_source source;

	if (output_name) {
		struct output *output_iter;

		wl_list_for_each(output_iter, &client->output_list, link) {
			if (!strcmp(output_name, output_iter->name)) {
				output = output_iter;
				break;
			}
		}

		test_assert_ptr_not_null(output);
	} else {
		output = client->output;
	}

	if (include_decorations == INCLUDE_DECORATIONS)
		source = WESTON_CAPTURE_V1_SOURCE_FULL_FRAMEBUFFER;
	else
		source = WESTON_CAPTURE_V1_SOURCE_FRAMEBUFFER;

	shm = client_capture_output(client, output, source,
				    CLIENT_BUFFER_TYPE_SHM);
	ih = image_header_from(shm->image);

	if (ih.pixman_format == PIXMAN_a8r8g8b8)
		return shm;

	buf = create_pixman_buffer(ih.width, ih.height, PIXMAN_a8r8g8b8);
	pixman_image_composite32(PIXMAN_OP_SRC, shm->image, NULL, buf->image,
				 0, 0, 0, 0, 0, 0, ih.width, ih.height);

	buffer_destroy(shm);
	return buf;
}

static void
write_visual_diff(pixman_image_t *ref_image,
		  pixman_image_t *shot,
		  const struct rectangle *clip,
		  int seq_no,
		  const struct pixel_diff_limit *lim)
{
	char *fname;
	pixman_image_t *diff;

	fname = output_filename_for_test_case("diff", seq_no, "png");
	diff = visualize_image_difference(ref_image, shot, clip, lim);
	write_image_as_png(diff, fname);

	pixman_image_unref(diff);
	free(fname);
}

/**
 * Verify image contents
 *
 * Compares the contents of the given shot to the given reference
 * image over the given clip rectangle, reports whether they match to the
 * test log, and if they do not match writes a visual diff into a PNG file
 * and the screenshot into another PNG file named with get_test_name() and
 * seq_no.
 *
 * The shot image size and the reference image size must both contain
 * the clip rectangle.
 *
 * If the error limits in \c vit are both exactly 0.0, then the internal
 * defaults of \c WESTON_VERIFY_IMAGE_AVG_LIMIT_DEFAULT and
 * \c WESTON_VERIFY_IMAGE_MAX_LIMIT_DEFAULT are used.
 *
 * \param shot The image to be verified, usually a screenshot.
 * \param vit The arguments; the reference image, the clip rectangle, error
 * limits.
 * \param seq_no Sequence number for writing output files on failure.
 * Among all the calls where \c get_test_name() would return the same string,
 * the number must be unique in each call.
 * \return True if the shot matches the reference image, false otherwise.
 *
 * \sa verify_screen_content
 */
bool
verify_image(pixman_image_t *shot,
	     const struct verify_image_target *vit,
	     int seq_no)
{
	struct two_norm_tolerance tol = {
		.max_limit = WESTON_VERIFY_IMAGE_MAX_LIMIT_DEFAULT,
		.avg_limit = WESTON_VERIFY_IMAGE_AVG_LIMIT_DEFAULT,
	};
	struct pixel_diff_limit lim = {
		.mode = ALPHA_COMPARE,
		.two_norm_limit = tol.max_limit,
	};
	pixman_image_t *ref = NULL;
	char *ref_fname = NULL;
	char *shot_fname;
	bool match = false;

	if (vit->tol.avg_limit != 0.0f || vit->tol.max_limit != 0.0f)
		tol = vit->tol;

	shot_fname = output_filename_for_test_case("shot", seq_no, "png");
	ref_fname = screenshot_reference_filename(vit->ref.basename, vit->ref.seq_no);
	ref = load_image_from_png(ref_fname);

	if (ref) {
		match = check_images_match(ref, shot, vit->clip, &tol);
		testlog("Verify reference image %s vs. shot %s: %s\n",
			ref_fname, shot_fname, match ? "PASS" : "FAIL");

		if (!match && tol.max_limit >= 0.0) {
			write_visual_diff(ref, shot, vit->clip, seq_no, &lim);
		}

		pixman_image_unref(ref);
	} else {
		testlog("No reference image, shot %s: FAIL\n", shot_fname);
	}

	if (!match)
		write_image_as_png(shot, shot_fname);

	free(ref_fname);
	free(shot_fname);

	return match;
}

/**
 * Take a screenshot and verify its contents
 *
 * Takes a screenshot and calls verify_image() with it.
 *
 * \param client The client, for connecting to the compositor.
 * \param vit The arguments to verify_image().
 * \param seq_no See verify_image().
 * \param output_name the output name as specified by wl_output.name. If NULL,
 * this is the last wl_output advertised by wl_registry.
 * \param include_decorations true if the screenshot should include output
 * decorations, or false if it should include just the client content
 * \return True if the screen contents matches the reference image,
 * false otherwise.
 *
 * For a simpler form, see \c verify_screen_content().
 */
bool
verify_output_content(struct client *client,
		      const struct verify_image_target *vit,
		      int seq_no,
		      const char *output_name,
		      enum screenshot_decoration_mode include_decorations)
{
	struct buffer *shot;
	bool match;

	shot = capture_screenshot_of_output(client, output_name,
					    include_decorations);
	test_assert_ptr_not_null(shot);
	match = verify_image(shot->image, vit, seq_no);
	buffer_destroy(shot);

	return match;
}

/**
 * Take a screenshot and verify its contents
 *
 * Takes a screenshot and calls verify_image() with it.
 *
 * \param client The client, for connecting to the compositor.
 * \param vit The arguments to verify_image().
 * \param seq_no See verify_image().
 * \return True if the screen contents matches the reference image,
 * false otherwise.
 *
 * Uses the last output advertised by wl_registry, and does not include
 * potential output decorations. If you need something else, see
 * \c verify_output_content().
 */
bool
verify_screen_content(struct client *client,
		      const struct verify_image_target *vit,
		      int seq_no)
{
	return verify_output_content(client, vit, seq_no, NULL, NO_DECORATIONS);
}

/**
 * Create a wl_buffer from a PNG file
 *
 * Loads the named PNG file from the directory of reference images,
 * creates a wl_buffer with scale times the image dimensions in pixels,
 * and copies the image content into the buffer using nearest-neighbor filter.
 *
 * \param client The client, for the Wayland connection.
 * \param basename The PNG file name without .png suffix.
 * \param scale Upscaling factor >= 1.
 */
struct buffer *
client_buffer_from_image_file(struct client *client,
			      const char *basename,
			      int scale)
{
	struct buffer *buf;
	char *fname;
	pixman_image_t *img;
	int buf_w, buf_h;
	pixman_transform_t scaling;

	test_assert_int_ge(scale, 1);

	fname = image_filename(basename);
	img = load_image_from_png(fname);
	free(fname);
	test_assert_ptr_not_null(img);

	buf_w = scale * pixman_image_get_width(img);
	buf_h = scale * pixman_image_get_height(img);
	buf = create_shm_buffer_a8r8g8b8(client, buf_w, buf_h);

	pixman_transform_init_scale(&scaling,
				    pixman_fixed_1 / scale,
				    pixman_fixed_1 / scale);
	pixman_image_set_transform(img, &scaling);
	pixman_image_set_filter(img, PIXMAN_FILTER_NEAREST, NULL, 0);

	pixman_image_composite32(PIXMAN_OP_SRC,
				 img, /* src */
				 NULL, /* mask */
				 buf->image, /* dst */
				 0, 0, /* src x,y */
				 0, 0, /* mask x,y */
				 0, 0, /* dst x,y */
				 buf_w, buf_h);
	pixman_image_unref(img);

	return buf;
}

/**
 * Create a solid color SHM buffer, attach it to the surface and commit.
 *
 * \param client The client instance.
 * \param surface The surface to which the buffer gets committed.
 * \param color The color of the solid SHM buffer.
 * \param width The width the buffer should have.
 * \param height The height the buffer should have.
 * \return The solid buffer committed. Should be destroyed by caller.
 */
struct buffer *
surface_commit_color(struct client *client, struct wl_surface *surface,
		     pixman_color_t *color, int width, int height)
{
	struct buffer *buf;

	buf = create_shm_buffer_solid(client, width, height, color);
	wl_surface_attach(surface, buf->proxy, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);

	return buf;
}

/**
 * Bind to a singleton global in wl_registry
 *
 * \param client Client whose registry and globals to use.
 * \param iface The Wayland interface to look for.
 * \param version The version to bind the interface with.
 * \return A struct wl_proxy, which you need to cast to the proper type.
 *
 * Asserts that the global being searched for is a singleton and is found.
 *
 * Binds with the exact version given, does not take compositor interface
 * version into account.
 */
void *
bind_to_singleton_global(struct client *client,
			 const struct wl_interface *iface,
			 int version)
{
	struct global *tmp;
	struct global *g = NULL;
	struct wl_proxy *proxy;

	wl_list_for_each(tmp, &client->global_list, link) {
		if (strcmp(tmp->interface, iface->name))
			continue;

		/* Multiple singleton objects. */
		test_assert_ptr_null(g);
		g = tmp;
	}

	/*  Singleton not found. */
	test_assert_ptr_not_null(g);

	proxy = wl_registry_bind(client->wl_registry, g->name, iface, version);
	test_assert_ptr_not_null(proxy);

	return proxy;
}

/**
 * Create a wp_viewport for the client surface
 *
 * \param client The client->surface to use.
 * \return A fresh viewport object.
 */
struct wp_viewport *
client_create_viewport(struct client *client)
{
	struct wp_viewport *viewport;

	viewport = wp_viewporter_get_viewport(client->viewporter,
					      client->surface->wl_surface);
	test_assert_ptr_not_null(viewport);

	return viewport;
}

struct buffer *
create_shm_buffer_solid(struct client *client, int width, int height,
			const pixman_color_t *color)
{
	struct buffer *buffer;

	buffer = create_shm_buffer_a8r8g8b8(client, width, height);
	if (!buffer)
		return NULL;
	fill_image_with_color(buffer->image, color);

	return buffer;
}

/**
 * Asks the server to wait for a specified breakpoint the next time it occurs,
 * synchronized as part of the protocol stream.
 *
 * \param client Client structure
 * \param suite_data Test suite data structure
 * \param breakpoint Breakpoint to stop at
 * \param proxy Optional breakpoint-specific object to filter by
 */
void
client_push_breakpoint(struct client *client,
		       struct wet_testsuite_data *suite_data,
		       enum weston_test_breakpoint breakpoint,
		       struct wl_proxy *proxy)
{
	weston_test_client_break(client->test->weston_test, breakpoint,
				 proxy ? wl_proxy_get_id(proxy) : 0);
}

/**
 * Waits for the server's next breakpoint and returns control to the client.
 * Must have had a corresponding client_push_breakpoint() call made before.
 *
 * May only be called after a weston_test.breakpoint request has been issued,
 * or within a break, before client_release_breakpoint() has been called.
 *
 * \param client Client structure
 * \param suite_data Test suite data structure
 * \return Information about the active breakpoint
 */
struct wet_test_active_breakpoint *
client_wait_breakpoint(struct client *client,
		       struct wet_testsuite_data *suite_data)
{
	struct wet_test_active_breakpoint *active_bp;

	test_assert_ptr_not_null(suite_data);
	test_assert_false(suite_data->breakpoints.in_client_break);

	wl_display_flush(client->wl_display);
	wet_test_wait_sem(&suite_data->breakpoints.client_break);

	active_bp = suite_data->breakpoints.active_bp;
	test_assert_ptr_not_null(active_bp);
	suite_data->breakpoints.in_client_break = true;
	return active_bp;
}

void *
get_resource_data_from_proxy(struct wet_testsuite_data *suite_data,
			     struct wl_proxy *proxy)
{
	struct wl_resource *resource;

	test_assert_true(suite_data->breakpoints.in_client_break);

	if (!proxy)
		return NULL;

	resource = wl_client_get_object(suite_data->wl_client,
					wl_proxy_get_id(proxy));
	test_assert_ptr_not_null(resource);
	return wl_resource_get_user_data(resource);
}

void
assert_resource_is_proxy(struct wet_testsuite_data *suite_data,
			 struct wl_resource *r, void *p)
{
	test_assert_ptr_not_null(r);
	test_assert_ptr_eq(wl_resource_get_client(r), suite_data->wl_client);
	test_assert_u32_eq(wl_resource_get_id(r),
			   wl_proxy_get_id((struct wl_proxy *) p));
}

void
assert_surface_matches(struct wet_testsuite_data *suite_data,
		       struct weston_surface *s, struct surface *c)
{
	test_assert_ptr_not_null(s);
	test_assert_ptr_not_null(c);

	assert_resource_is_proxy(suite_data, s->resource, c->wl_surface);
	test_assert_s32_eq(s->width, c->width);
	test_assert_s32_eq(s->height, c->height);

	test_assert_ptr_not_null(s->buffer_ref.buffer);
	test_assert_ptr_not_null(c->buffer);
	assert_resource_is_proxy(suite_data, s->buffer_ref.buffer->resource,
				 c->buffer->proxy);
}

void
assert_output_matches(struct wet_testsuite_data *suite_data,
		      struct weston_output *s, struct output *c)
{
	struct weston_head *head;
	bool found_client_resource = false;

	test_assert_ptr_not_null(s);
	test_assert_ptr_not_null(c);

	wl_list_for_each(head, &s->head_list, output_link) {
		struct wl_resource *res;
		wl_resource_for_each(res, &head->resource_list) {
			if (wl_resource_get_client(res) == suite_data->wl_client &&
			    wl_resource_get_id(res) ==
			     wl_proxy_get_id((struct wl_proxy *) c->wl_output)) {
				found_client_resource = true;
				break;
			}
		}
	}
	test_assert_true(found_client_resource);

	test_assert_s32_eq(s->width, c->width);
	test_assert_s32_eq(s->height, c->height);
}

/**
 * Asks the server to wait for a specified breakpoint the next time it occurs,
 * inserted immediately into the wait list with no synchronization to the
 * protocol stream.
 *
 * Must only be called between client_wait_breakpoint() and
 * client_release_breakpoint().
 *
 * \param client Client structure
 * \param suite_data Test suite data structure
 * \param breakpoint Breakpoint to stop at
 * \param proxy Optional breakpoint-specific object to filter by
 */
void
client_insert_breakpoint(struct client *client,
		         struct wet_testsuite_data *suite_data,
			 enum weston_test_breakpoint breakpoint,
			 struct wl_proxy *proxy)
{
	struct wet_test_pending_breakpoint *bp;

	test_assert_true(suite_data->breakpoints.in_client_break);

	bp = xzalloc(sizeof(*bp));
	bp->breakpoint = breakpoint;
	bp->resource = get_resource_data_from_proxy(suite_data, proxy);
	wl_list_insert(&suite_data->breakpoints.list, &bp->link);
}

/**
 * Removes a specified breakpoint from the server's breakpoint list,
 * with no synchronization to the protocol stream.
 *
 * Must only be called between client_wait_breakpoint() and
 * client_release_breakpoint().
 *
 * \param client Client structure
 * \param suite_data Test suite data structure
 * \param breakpoint Breakpoint to remove
 * \param proxy Optional breakpoint-specific object to filter by
 */
void
client_remove_breakpoint(struct client *client,
		         struct wet_testsuite_data *suite_data,
			 enum weston_test_breakpoint breakpoint,
			 struct wl_proxy *proxy)
{
	struct wet_test_pending_breakpoint *bp, *tmp;
	void *resource = get_resource_data_from_proxy(suite_data, proxy);

	test_assert_true(suite_data->breakpoints.in_client_break);

	wl_list_for_each_safe(bp, tmp, &suite_data->breakpoints.list,
			      link) {
		if (bp->breakpoint != breakpoint)
			continue;
		if (bp->resource != resource)
			continue;
		test_assert_ptr_ne(bp, suite_data->breakpoints.active_bp->template_);
		wl_list_remove(&bp->link);
		free(bp);
		return;
	}

	test_assert_not_reached("couldn't find breakpoint to remove");
}

/**
 * Continues server execution after a breakpoint.
 *
 * \param client Client structure
 * \param suite_data Test suite data structure
 * \param active_bp Data structure returned from client_wait_breakpoint()
 */
void
client_release_breakpoint(struct client *client,
			  struct wet_testsuite_data *suite_data,
			  struct wet_test_active_breakpoint *active_bp)
{
	test_assert_ptr_eq(suite_data->breakpoints.active_bp, active_bp);

	if (active_bp->rearm_on_release) {
		wl_list_insert(&suite_data->breakpoints.list,
			       &active_bp->template_->link);
	} else {
		free(active_bp->template_);
	}

	free(active_bp);
	suite_data->breakpoints.active_bp = NULL;
	suite_data->breakpoints.in_client_break = false;
	wet_test_post_sem(&suite_data->breakpoints.server_release);
}

struct wl_subcompositor *
client_get_subcompositor(struct client *client)
{
	struct global *g;
	struct global *global_sub = NULL;
	struct wl_subcompositor *sub;

	wl_list_for_each(g, &client->global_list, link) {
		if (strcmp(g->interface, "wl_subcompositor"))
			continue;

		if (global_sub)
			test_assert_not_reached("multiple wl_subcompositor objects");

		global_sub = g;
	}

	test_assert_ptr_not_null(global_sub);

	test_assert_u32_eq(global_sub->version, 1);

	sub = wl_registry_bind(client->wl_registry, global_sub->name,
			      &wl_subcompositor_interface, 1);
	test_assert_ptr_not_null(sub);

	return sub;
}

static void
presentation_clock_id(void *data, struct wp_presentation *presentation,
		      uint32_t clk_id)
{
	struct client *client = data;

	client->presentation_clock = clk_id;
	client->has_presentation_clock = true;
}

static const struct wp_presentation_listener presentation_listener = {
	presentation_clock_id
};

struct wp_presentation *
client_get_presentation(struct client *client)
{
	struct global *g;
	struct global *global_pres = NULL;
	struct wp_presentation *pres;

	wl_list_for_each(g, &client->global_list, link) {
		if (strcmp(g->interface, wp_presentation_interface.name))
			continue;

		if (global_pres)
			test_assert_not_reached("multiple presentation objects");

		global_pres = g;
	}

	test_assert_ptr_not_null(global_pres);

	test_assert_u32_eq(global_pres->version, 2);

	pres = wl_registry_bind(client->wl_registry, global_pres->name,
				&wp_presentation_interface, 2);
	wp_presentation_add_listener(pres, &presentation_listener, client);

	test_assert_ptr_not_null(pres);

	client_roundtrip(client);

	return pres;
}

clockid_t
client_get_presentation_clock(struct client *client)
{
	test_assert_true(client->has_presentation_clock);

	return client->presentation_clock;
}
