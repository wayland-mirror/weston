/*
 * Sergio Gómez for Collabora Inc.
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

#include <linux/input.h>
#include <wayland-client-protocol.h>

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "shared/timespec-util.h"
#include "weston-test-client-helper.h"
#include "weston-test-client-protocol.h"
#include "weston-test-assert.h"

struct constraints {
	struct zwp_pointer_constraints_v1 *zwp_pointer_constraints;
	struct zwp_confined_pointer_v1 *zwp_confined_pointer;
	struct zwp_locked_pointer_v1 *zwp_locked_pointer;
	bool pointer_is_confined;
	bool pointer_is_locked;
};

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.shell = SHELL_TEST_DESKTOP;
	setup.refresh = HIGHEST_OUTPUT_REFRESH;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);

static void
click_pointer(struct client *client)
{
	weston_test_send_button(client->test->weston_test, 0, 0, 0, BTN_LEFT,
				WL_POINTER_BUTTON_STATE_PRESSED);
	weston_test_send_button(client->test->weston_test, 0, 0, 0, BTN_LEFT,
				WL_POINTER_BUTTON_STATE_RELEASED);
	client_roundtrip(client);
}

static void
move_pointer(struct client *client, int x, int y)
{
	weston_test_move_pointer(client->test->weston_test, 0, 0, 0, x, y);
	client_roundtrip(client);
}

static void
pointer_locked_event(void *data, struct zwp_locked_pointer_v1 *locked_pointer)
{
	struct constraints *cs = data;

	test_assert_ptr_eq(locked_pointer, cs->zwp_locked_pointer);
	cs->pointer_is_locked = true;
}

static void
pointer_unlocked_event(void *data, struct zwp_locked_pointer_v1 *locked_pointer)
{
	struct constraints *cs = data;

	test_assert_ptr_eq(locked_pointer, cs->zwp_locked_pointer);
	cs->pointer_is_locked = false;
}

static struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
	pointer_locked_event,
	pointer_unlocked_event,
};

static void
pointer_confined_event(void *data, struct zwp_confined_pointer_v1 *confined_pointer)
{
	struct constraints *cs = data;

	test_assert_ptr_eq(confined_pointer, cs->zwp_confined_pointer);
	cs->pointer_is_confined = true;
}

static void
pointer_unconfined_event(void *data, struct zwp_confined_pointer_v1 *confined_pointer)
{
	struct constraints *cs = data;

	test_assert_ptr_eq(confined_pointer, cs->zwp_confined_pointer);
	cs->pointer_is_confined = false;
}

static struct zwp_confined_pointer_v1_listener confined_pointer_listener = {
	pointer_confined_event,
	pointer_unconfined_event,
};

static void
lock_pointer(struct constraints *cs, struct client *client,
	     struct zwp_locked_pointer_v1_listener *listener,
	     struct wl_region *region,
	     enum zwp_pointer_constraints_v1_lifetime lifetime)
{
	cs->zwp_locked_pointer =
		zwp_pointer_constraints_v1_lock_pointer(cs->zwp_pointer_constraints,
							client->surface->wl_surface,
							client->input->pointer->wl_pointer,
							region,
							lifetime);
	zwp_locked_pointer_v1_add_listener(cs->zwp_locked_pointer, listener, cs);
	client_roundtrip(client);
}

static void
lock_destroy(struct constraints *cs, struct client *client)
{
	zwp_locked_pointer_v1_destroy(cs->zwp_locked_pointer);
	cs->zwp_locked_pointer = NULL;
	cs->pointer_is_locked = false;
	client_roundtrip(client);
}

static void
confine_pointer(struct constraints *cs, struct client *client,
		struct zwp_confined_pointer_v1_listener *listener,
		struct wl_region *region,
		enum zwp_pointer_constraints_v1_lifetime lifetime)
{
	cs->zwp_confined_pointer =
		zwp_pointer_constraints_v1_confine_pointer(cs->zwp_pointer_constraints,
							   client->surface->wl_surface,
							   client->input->pointer->wl_pointer,
							   region,
							   lifetime);
	zwp_confined_pointer_v1_add_listener(cs->zwp_confined_pointer, listener, cs);
	client_roundtrip(client);
}

static void
confine_destroy(struct constraints *cs, struct client *client)
{
	zwp_confined_pointer_v1_destroy(cs->zwp_confined_pointer);
	cs->zwp_confined_pointer = NULL;
	cs->pointer_is_confined = false;
	client_roundtrip(client);
}

static void
constraints_init(struct constraints *cs, struct client *client)
{
	cs->zwp_pointer_constraints = bind_to_singleton_global(client,
							       &zwp_pointer_constraints_v1_interface,
							       1);
	test_assert_ptr_not_null(cs->zwp_pointer_constraints);
}

static void
constraint_deinit(struct constraints *cs)
{
	if (cs->zwp_confined_pointer) {
		zwp_confined_pointer_v1_destroy(cs->zwp_confined_pointer);
		cs->zwp_confined_pointer = NULL;
	}
	if (cs->zwp_locked_pointer) {
		zwp_locked_pointer_v1_destroy(cs->zwp_locked_pointer);
		cs->zwp_locked_pointer = NULL;
	}
	if (cs->zwp_pointer_constraints) {
		zwp_pointer_constraints_v1_destroy(cs->zwp_pointer_constraints);
		cs->zwp_pointer_constraints = NULL;
	}
}

static enum test_result_code
constraints_events(struct wet_testsuite_data *suite_data)
{
	static struct constraints cs;
	struct client *client;

	client = create_client_and_test_surface(100, 100, 100, 100);
	constraints_init(&cs, client);

	/* activate surface */
	move_pointer(client, 150, 150);
	click_pointer(client);

	/* receive confined events for oneshot lifetime */
	confine_pointer(&cs, client, &confined_pointer_listener, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_confined);
	confine_destroy(&cs, client);

	/* receive confined events for persistent lifetime */
	confine_pointer(&cs, client, &confined_pointer_listener, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	test_assert_true(cs.pointer_is_confined);
	confine_destroy(&cs, client);

	/* receive locked events for oneshot lifetime */
	lock_pointer(&cs, client, &locked_pointer_listener, NULL,
		     ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_locked);
	lock_destroy(&cs, client);

	/* receive locked events for persistent lifetime */
	lock_pointer(&cs, client, &locked_pointer_listener, NULL,
		     ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	test_assert_true(cs.pointer_is_locked);
	lock_destroy(&cs, client);

	constraint_deinit(&cs);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
constraints_confined_boundaries_input_region(struct wet_testsuite_data *suite_data)
{
	static struct constraints cs;
	struct client *client;

	client = create_client_and_test_surface(100, 100, 100, 100);
	constraints_init(&cs, client);

	/* activate surface */
	move_pointer(client, 150, 150);
	click_pointer(client);

	/* confine to whole surface */
	confine_pointer(&cs, client, &confined_pointer_listener, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_confined);

	/* move to boundary */
	move_pointer(client, 100, 100);

	/* x-1 (outside boundary) */
	move_pointer(client, client->surface->x-1, client->surface->y);
	test_assert_ptr_eq(client->input->pointer->focus, client->surface);
	test_assert_int_eq(client->test->pointer_x, client->surface->x);

	/* y-1 (outside boundary) */
	move_pointer(client, client->surface->x, client->surface->y-1);
	test_assert_ptr_eq(client->input->pointer->focus, client->surface);
	test_assert_int_eq(client->test->pointer_y, client->surface->y);

	/* x+width (outside boundary) */
	move_pointer(client, client->surface->x+client->surface->width,
		     client->surface->y);
	test_assert_ptr_eq(client->input->pointer->focus, client->surface);
	test_assert_int_eq(client->test->pointer_x,
			   client->surface->x+client->surface->width-1);

	/* y+height (outside boundary) */
	move_pointer(client, client->surface->x,
		     client->surface->y+client->surface->height);
	test_assert_ptr_eq(client->input->pointer->focus, client->surface);
	test_assert_int_eq(client->test->pointer_y,
			   client->surface->y+client->surface->height-1);

	confine_destroy(&cs, client);
	/* x-1 (after unconfinement) */
	move_pointer(client, client->surface->x-1, client->surface->y);
	test_assert_ptr_ne(client->input->pointer->focus, client->surface);
	test_assert_int_eq(client->test->pointer_x, client->surface->x-1);

	constraint_deinit(&cs);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
constraints_locked_boundaries_input_region(struct wet_testsuite_data *suite_data)
{
	static struct constraints cs;
	struct client *client;

	client = create_client_and_test_surface(100, 100, 100, 100);
	constraints_init(&cs, client);

	/* activate surface */
	move_pointer(client, 100, 100);
	click_pointer(client);

	lock_pointer(&cs, client, &locked_pointer_listener, NULL,
		     ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_locked);

	/* x-1 (outside surface) */
	move_pointer(client, client->surface->x-1, client->surface->y);
	test_assert_ptr_eq(client->input->pointer->focus, client->surface);
	test_assert_int_eq(client->test->pointer_x, client->surface->x);

	/* x+1 (inside surface) */
	move_pointer(client, client->surface->x+1, client->surface->y);
	test_assert_ptr_eq(client->input->pointer->focus, client->surface);
	test_assert_int_eq(client->test->pointer_x, client->surface->x);

	lock_destroy(&cs, client);
	/* x-1 (after unlocking) */
	move_pointer(client, client->surface->x-1, client->surface->y);
	test_assert_ptr_ne(client->input->pointer->focus, client->surface);
	test_assert_int_eq(client->test->pointer_x, client->surface->x-1);

	constraint_deinit(&cs);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
constraints_already_constrained(struct wet_testsuite_data *suite_data)
{
	static struct constraints cs;
	struct client *client;

	client = create_client_and_test_surface(100, 100, 100, 100);
	constraints_init(&cs, client);

	/* activate surface */
	move_pointer(client, 150, 150);
	click_pointer(client);

	/* try to lock an already confined pointer */
	confine_pointer(&cs, client, &confined_pointer_listener, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_confined);
	cs.zwp_locked_pointer =
		zwp_pointer_constraints_v1_lock_pointer(cs.zwp_pointer_constraints,
							client->surface->wl_surface,
							client->input->pointer->wl_pointer,
							NULL,
							ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	zwp_locked_pointer_v1_add_listener(cs.zwp_locked_pointer, &locked_pointer_listener, &cs);
	expect_protocol_error(client, &zwp_pointer_constraints_v1_interface,
			      ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED);

	constraint_deinit(&cs);
	client_destroy(client);

	client = create_client_and_test_surface(100, 100, 100, 100);
	constraints_init(&cs, client);

	/* activate surface */
	move_pointer(client, 150, 150);
	click_pointer(client);

	/* try to confine an already locked pointer */
	lock_pointer(&cs, client, &locked_pointer_listener, NULL,
		     ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_locked);
	cs.zwp_confined_pointer =
		zwp_pointer_constraints_v1_confine_pointer(cs.zwp_pointer_constraints,
							   client->surface->wl_surface,
							   client->input->pointer->wl_pointer,
							   NULL,
							   ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	zwp_confined_pointer_v1_add_listener(cs.zwp_confined_pointer,
					     &confined_pointer_listener, &cs);
	expect_protocol_error(client, &zwp_pointer_constraints_v1_interface,
			      ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED);

	constraint_deinit(&cs);
	client_destroy(client);

	return RESULT_OK;
}

/*
 * Activation comes from the shell logic.
 * Although we are not using the desktop shell in the tests,
 * weston-test-desktop-shell provides what's necessary to simulate the behaviour
 * we are interested in: enable the constraint on activation only when it comes
 * from a pointer click inside the surface.
 */
static enum test_result_code
constraints_shell_activate_input(struct wet_testsuite_data *suite_data)
{
	static struct constraints cs;
	struct client *client;

	client = create_client_and_test_surface(100, 100, 100, 100);
	constraints_init(&cs, client);
	move_pointer(client, 150, 150);

	confine_pointer(&cs, client, &confined_pointer_listener, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_false(cs.pointer_is_confined);

	/*
	 * This mimics the desktop shell when activating input for the view in
	 * any case other than clicking inside the surface.
	 */
	weston_test_activate_surface(client->test->weston_test,
				     client->surface->wl_surface);
	client_roundtrip(client);
	test_assert_false(cs.pointer_is_confined);

	/* activation that comes from clicking inside the surface */
	click_pointer(client);
	client_roundtrip(client);
	test_assert_true(cs.pointer_is_confined);

	constraint_deinit(&cs);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
constraints_pointer_focus(struct wet_testsuite_data *suite_data)
{
	static struct constraints cs;
	struct client *client;

	client = create_client_and_test_surface(100, 100, 100, 100);
	constraints_init(&cs, client);

	confine_pointer(&cs, client, &confined_pointer_listener, NULL,
			ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_false(cs.pointer_is_confined);

	/* focus out */
	move_pointer(client, 0, 0);

	/* focus in: should not confine */
	move_pointer(client, 150, 150);
	client_roundtrip(client);
	test_assert_false(cs.pointer_is_confined);

	/* confine */
	click_pointer(client);
	test_assert_true(cs.pointer_is_confined);

	/* focus out: should not unconfine */
	move_pointer(client, 0, 0);
	test_assert_true(cs.pointer_is_confined);

	constraint_deinit(&cs);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
constraints_locked_cursor_hint(struct wet_testsuite_data *suite_data)
{
	static struct constraints cs;
	struct client *client;

	client = create_client_and_test_surface(100, 100, 100, 100);
	constraints_init(&cs, client);

	/* activate surface */
	move_pointer(client, 150, 150);
	click_pointer(client);

	/* A hint that was never committed must be ignored on unlock. */
	lock_pointer(&cs, client, &locked_pointer_listener, NULL,
		     ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_locked);
	zwp_locked_pointer_v1_set_cursor_position_hint(cs.zwp_locked_pointer,
						       wl_fixed_from_int(10),
						       wl_fixed_from_int(20));
	lock_destroy(&cs, client);

	/* While locked, the reported position is the current one. */
	lock_pointer(&cs, client, &locked_pointer_listener, NULL,
		     ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_locked);
	move_pointer(client, 0, 0);
	test_assert_int_eq(client->test->pointer_x, 150);
	test_assert_int_eq(client->test->pointer_y, 150);

	/* A committed hint is used on unlock. */
	zwp_locked_pointer_v1_set_cursor_position_hint(cs.zwp_locked_pointer,
						       wl_fixed_from_int(10),
						       wl_fixed_from_int(20));
	wl_surface_commit(client->surface->wl_surface);
	lock_destroy(&cs, client);

	lock_pointer(&cs, client, &locked_pointer_listener, NULL,
		     ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
	test_assert_true(cs.pointer_is_locked);
	move_pointer(client, 0, 0);
	test_assert_int_eq(client->test->pointer_x, client->surface->x + 10);
	test_assert_int_eq(client->test->pointer_y, client->surface->y + 20);

	constraint_deinit(&cs);
	client_destroy(client);

	return RESULT_OK;
}

DECLARE_TEST_LIST(
	TESTFN(constraints_events),
	TESTFN(constraints_confined_boundaries_input_region),
	TESTFN(constraints_locked_boundaries_input_region),
	TESTFN(constraints_already_constrained),
	TESTFN(constraints_shell_activate_input),
	TESTFN(constraints_pointer_focus),
	TESTFN(constraints_locked_cursor_hint),
);
