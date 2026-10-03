// SPDX-License-Identifier: GPL-2.0-only
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include "input/cursor.h"
#include "labwc.h"
#include "pointer-warp-v1-protocol.h"

static uint32_t enter_serial;
static struct wl_listener focus_change;
static struct wl_listener seat_destroy;

static void
handle_focus_change(struct wl_listener *listener, void *data)
{
	struct wlr_seat_pointer_focus_change_event *event = data;
	struct wlr_seat_client *client = event->seat->pointer_state.focused_client;
	if (!event->new_surface || !client || client->serials.count == 0) {
		return;
	}
	enter_serial = client->serials.data[client->serials.end].max_incl;
}

static void
handle_seat_destroy(struct wl_listener *listener, void *data)
{
	wl_list_remove(&focus_change.link);
	wl_list_remove(&seat_destroy.link);
}

static void
handle_warp_pointer(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *surface_resource, struct wl_resource *pointer_resource,
		wl_fixed_t fx, wl_fixed_t fy, uint32_t serial)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct wlr_seat_client *seat_client =
		wlr_seat_client_from_pointer_resource(pointer_resource);
	struct seat *seat = &server.seat;
	struct wlr_seat *wlr_seat = seat->wlr_seat;
	double sx = wl_fixed_to_double(fx);
	double sy = wl_fixed_to_double(fy);

	if (!seat_client || seat_client->seat != wlr_seat
			|| wlr_seat->pointer_state.focused_client != seat_client
			|| wlr_seat->pointer_state.focused_surface != surface
			|| serial != enter_serial
			|| sx < 0 || sy < 0
			|| sx >= surface->current.width
			|| sy >= surface->current.height
			|| server.input_mode != LAB_INPUT_STATE_PASSTHROUGH) {
		wlr_log(WLR_DEBUG, "rejected pointer warp request");
		return;
	}

	double lx = seat->cursor->x - wlr_seat->pointer_state.sx + sx;
	double ly = seat->cursor->y - wlr_seat->pointer_state.sy + sy;
	wlr_cursor_warp(seat->cursor, NULL, lx, ly);

	cursor_update_focus();

	if (wlr_seat->pointer_state.focused_surface == surface) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		uint32_t time_msec = now.tv_sec * 1000 + now.tv_nsec / 1000000;
		wlr_seat_pointer_notify_motion(wlr_seat, time_msec,
			seat->cursor->x - lx + sx, seat->cursor->y - ly + sy);
		wlr_seat_pointer_notify_frame(wlr_seat);
	}
}

static void
handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wp_pointer_warp_v1_interface pointer_warp_impl = {
	.destroy = handle_destroy,
	.warp_pointer = handle_warp_pointer,
};

static void
pointer_warp_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_pointer_warp_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &pointer_warp_impl, NULL, NULL);
}

void
pointer_warp_manager_create(struct wl_display *display)
{
	if (!wl_global_create(display, &wp_pointer_warp_v1_interface, 1, NULL,
			pointer_warp_bind)) {
		wlr_log(WLR_ERROR, "failed to create wp_pointer_warp_v1 global");
		return;
	}
	focus_change.notify = handle_focus_change;
	wl_signal_add(&server.seat.wlr_seat->pointer_state.events.focus_change,
		&focus_change);
	seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&server.seat.wlr_seat->events.destroy, &seat_destroy);
}
