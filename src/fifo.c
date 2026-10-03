// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include "common/mem.h"
#include "fifo-v1-protocol.h"
#include "labwc.h"

struct fifo_state {
	bool set_barrier;
	bool wait_barrier;
};

struct fifo {
	struct wlr_surface *surface;
	struct wl_resource *resource;
	struct wlr_addon addon;
	struct wlr_surface_synced synced;
	struct fifo_state pending, current;

	bool barrier;
	struct wl_array locks;

	struct wl_listener client_commit;
	struct wl_list link;
};

static struct wl_list fifos = { &fifos, &fifos };

static const struct wp_fifo_v1_interface fifo_impl;

static struct fifo *
fifo_from_resource(struct wl_resource *resource)
{
	assert(wl_resource_instance_of(resource, &wp_fifo_v1_interface, &fifo_impl));
	return wl_resource_get_user_data(resource);
}

static void
fifo_release(struct fifo *fifo)
{
	while (fifo->locks.size > 0 && !fifo->barrier) {
		uint32_t *seqs = fifo->locks.data;
		uint32_t seq = seqs[0];
		fifo->locks.size -= sizeof(seq);
		memmove(seqs, seqs + 1, fifo->locks.size);
		wlr_surface_unlock_cached(fifo->surface, seq);
	}
}

static bool
surface_is_synchronized(struct wlr_surface *surface)
{
	struct wlr_subsurface *subsurface;
	while (surface && (subsurface = wlr_subsurface_try_from_wlr_surface(surface))) {
		if (subsurface->synchronized) {
			return true;
		}
		surface = subsurface->parent;
	}
	return false;
}

static void
handle_client_commit(struct wl_listener *listener, void *data)
{
	struct fifo *fifo = wl_container_of(listener, fifo, client_commit);
	struct wlr_surface *surface = fifo->surface;

	if (!fifo->pending.wait_barrier) {
		return;
	}
	if (!fifo->barrier && fifo->locks.size == 0) {
		return;
	}
	if (surface_is_synchronized(surface)) {
		return;
	}
	if (wl_list_empty(&surface->current_outputs)) {
		return;
	}

	uint32_t *seq = wl_array_add(&fifo->locks, sizeof(*seq));
	if (!seq) {
		wlr_log(WLR_ERROR, "fifo: failed to queue commit");
		return;
	}
	*seq = wlr_surface_lock_pending(surface);

	struct wlr_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		wlr_output_schedule_frame(surface_output->output);
	}
}

void
fifo_output_frame(struct wlr_output *output)
{
	struct fifo *fifo, *tmp;
	wl_list_for_each_safe(fifo, tmp, &fifos, link) {
		bool on_output = wl_list_empty(&fifo->surface->current_outputs);
		struct wlr_surface_output *surface_output;
		wl_list_for_each(surface_output, &fifo->surface->current_outputs, link) {
			on_output |= surface_output->output == output;
		}
		if (on_output) {
			fifo->barrier = false;
			fifo_release(fifo);
		}
	}
}

static void
synced_move_state(void *_dst, void *_src)
{
	struct fifo_state *dst = _dst, *src = _src;
	*dst = *src;
	*src = (struct fifo_state){0};
}

static void
synced_commit(struct wlr_surface_synced *synced)
{
	struct fifo *fifo = wl_container_of(synced, fifo, synced);
	if (fifo->current.set_barrier) {
		fifo->barrier = true;
	}
}

static const struct wlr_surface_synced_impl synced_impl = {
	.state_size = sizeof(struct fifo_state),
	.move_state = synced_move_state,
	.commit = synced_commit,
};

static void
addon_destroy(struct wlr_addon *addon)
{
	struct fifo *fifo = wl_container_of(addon, fifo, addon);
	if (fifo->resource) {
		wl_resource_set_user_data(fifo->resource, NULL);
	}
	wl_list_remove(&fifo->client_commit.link);
	wlr_surface_synced_finish(&fifo->synced);
	wlr_addon_finish(&fifo->addon);
	wl_array_release(&fifo->locks);
	wl_list_remove(&fifo->link);
	free(fifo);
}

static const struct wlr_addon_interface addon_impl = {
	.name = "labwc_fifo_v1",
	.destroy = addon_destroy,
};

static void
fifo_handle_set_barrier(struct wl_client *client, struct wl_resource *resource)
{
	struct fifo *fifo = fifo_from_resource(resource);
	if (!fifo) {
		wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
			"surface destroyed");
		return;
	}
	fifo->pending.set_barrier = true;
}

static void
fifo_handle_wait_barrier(struct wl_client *client, struct wl_resource *resource)
{
	struct fifo *fifo = fifo_from_resource(resource);
	if (!fifo) {
		wl_resource_post_error(resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED,
			"surface destroyed");
		return;
	}
	fifo->pending.wait_barrier = true;
}

static void
fifo_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wp_fifo_v1_interface fifo_impl = {
	.set_barrier = fifo_handle_set_barrier,
	.wait_barrier = fifo_handle_wait_barrier,
	.destroy = fifo_handle_destroy,
};

static void
fifo_handle_resource_destroy(struct wl_resource *resource)
{
	struct fifo *fifo = fifo_from_resource(resource);
	if (fifo) {
		fifo->resource = NULL;
	}
}

static struct fifo *
fifo_get_or_create(struct wlr_surface *surface)
{
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL, &addon_impl);
	if (addon) {
		struct fifo *fifo = wl_container_of(addon, fifo, addon);
		return fifo;
	}

	struct fifo *fifo = znew(*fifo);
	if (!wlr_surface_synced_init(&fifo->synced, surface, &synced_impl,
			&fifo->pending, &fifo->current)) {
		free(fifo);
		return NULL;
	}
	fifo->surface = surface;
	wl_array_init(&fifo->locks);
	wlr_addon_init(&fifo->addon, &surface->addons, NULL, &addon_impl);
	fifo->client_commit.notify = handle_client_commit;
	wl_signal_add(&surface->events.client_commit, &fifo->client_commit);
	wl_list_insert(&fifos, &fifo->link);
	return fifo;
}

static void
manager_handle_get_fifo(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct fifo *fifo = fifo_get_or_create(surface);
	if (!fifo) {
		wl_client_post_no_memory(client);
		return;
	}
	if (fifo->resource) {
		wl_resource_post_error(resource, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS,
			"a wp_fifo_v1 object already exists for this surface");
		return;
	}

	fifo->resource = wl_resource_create(client, &wp_fifo_v1_interface,
		wl_resource_get_version(resource), id);
	if (!fifo->resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(fifo->resource, &fifo_impl, fifo,
		fifo_handle_resource_destroy);
}

static void
manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wp_fifo_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_fifo = manager_handle_get_fifo,
};

static void
manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&wp_fifo_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, NULL, NULL);
}

void
fifo_manager_create(struct wl_display *display)
{
	if (!wl_global_create(display, &wp_fifo_manager_v1_interface, 1, NULL,
			manager_bind)) {
		wlr_log(WLR_ERROR, "failed to create wp_fifo_manager_v1 global");
	}
}
