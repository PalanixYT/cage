/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2020 Jente Hidskes
 * Copyright (C) 2020 The Sway authors
 *
 * See the LICENSE file accompanying this file.
 */

#include "layer_shell_v1.h"
#include "server.h"

#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>

static void
handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *cg_layer = wl_container_of(listener, cg_layer, output_destroy);

	wl_list_remove(&cg_layer->output_destroy.link);
	wl_list_remove(&cg_layer->link);
	wl_list_init(&cg_layer->link);

	cg_layer->layer_surface->output = NULL;
	wlr_layer_surface_v1_close(cg_layer->layer_surface);
}

static void
unmap(struct cg_layer_surface *cg_layer)
{
	struct wlr_output *wlr_output = cg_layer->layer_surface->output;
	if (!wlr_output) {
		return;
	}

	struct cg_output *output = wlr_output->data;
	if (!output) {
		return;
	}

	output_damage_surface(output, cg_layer->layer_surface->surface, cg_layer->geometry.x, cg_layer->geometry.y,
			      true);
}

static void
handle_destroy(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *cg_layer = wl_container_of(listener, cg_layer, destroy);

	wlr_log(WLR_DEBUG, "Layer surface destroyed (%s)", cg_layer->layer_surface->namespace);

	if (cg_layer->layer_surface->mapped) {
		unmap(cg_layer);
	}

	wl_list_remove(&cg_layer->link);
	wl_list_remove(&cg_layer->map.link);
	wl_list_remove(&cg_layer->unmap.link);
	wl_list_remove(&cg_layer->surface_commit.link);
	wl_list_remove(&cg_layer->destroy.link);

	if (cg_layer->layer_surface->output) {
		wl_list_remove(&cg_layer->output_destroy.link);
		cg_layer->layer_surface->output = NULL;
	}

	free(cg_layer);
}

static void
handle_surface_commit(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *layer = wl_container_of(listener, layer, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
	struct wlr_output *wlr_output = layer_surface->output;

	if (!wlr_output) {
		return;
	}

	struct cg_output *output = wlr_output->data;
	struct wlr_box old_geometry = layer->geometry;

	bool geometry_changed = memcmp(&old_geometry, &layer->geometry, sizeof(struct wlr_box)) != 0;
	bool layer_changed = layer->layer != layer_surface->current.layer;

	if (layer_changed) {
		wl_list_remove(&layer->link);
		wl_list_insert(&output->layers[layer_surface->current.layer], &layer->link);
		layer->layer = layer_surface->current.layer;
	}

	if (geometry_changed || layer_changed) {
		output_damage_surface(output, layer_surface->surface, old_geometry.x, old_geometry.y, true);
		output_damage_surface(output, layer_surface->surface, layer->geometry.x, layer->geometry.y, true);
	} else {
		output_damage_surface(output, layer_surface->surface, layer->geometry.x, layer->geometry.y, false);
	}
}

static void
handle_unmap(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *cg_layer = wl_container_of(listener, cg_layer, unmap);
	unmap(cg_layer);
}

static void
handle_map(struct wl_listener *listener, void *data)
{
	struct cg_layer_surface *cg_layer = wl_container_of(listener, cg_layer, map);
	struct cg_output *output = cg_layer->layer_surface->output->data;
	output_damage_surface(output, cg_layer->layer_surface->surface, cg_layer->geometry.x, cg_layer->geometry.y,
			      true);
	wlr_surface_send_enter(cg_layer->layer_surface->surface, cg_layer->layer_surface->output);
}

void
handle_layer_shell_v1_surface_new(struct wl_listener *listener, void *data)
{
	struct cg_server *server = wl_container_of(listener, server, new_layer_shell_v1_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	wlr_log(WLR_DEBUG, "New layer shell surface: namespace %s layer %d anchor %d size %dx%d margin %d,%d,%d,%d",
		layer_surface->namespace, layer_surface->client_pending.layer, layer_surface->client_pending.layer,
		layer_surface->client_pending.desired_width, layer_surface->client_pending.desired_height,
		layer_surface->client_pending.margin.top, layer_surface->client_pending.margin.right,
		layer_surface->client_pending.margin.bottom, layer_surface->client_pending.margin.left);

	/* If the layer surface doesn't specify an output, we assign the only output explicitly. */
	if (!layer_surface->output) {
		layer_surface->output = server->output->wlr_output;
	}

	struct cg_layer_surface *cg_layer = calloc(1, sizeof(struct cg_layer_surface));
	if (!cg_layer) {
		wlr_log(WLR_ERROR, "Failed to allocate layer shell");
		return;
	}

	cg_layer->server = server;
	cg_layer->layer_surface = layer_surface;
	layer_surface->data = cg_layer;

	cg_layer->map.notify = handle_map;
	wl_signal_add(&layer_surface->events.map, &cg_layer->map);
	cg_layer->unmap.notify = handle_unmap;
	wl_signal_add(&layer_surface->events.unmap, &cg_layer->unmap);
	cg_layer->surface_commit.notify = handle_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &cg_layer->surface_commit);
	cg_layer->destroy.notify = handle_destroy;
	wl_signal_add(&layer_surface->events.destroy, &cg_layer->destroy);

	struct cg_output *output = layer_surface->output->data;
	cg_layer->output_destroy.notify = handle_output_destroy;
	wl_signal_add(&output->wlr_output->events.destroy, &cg_layer->output_destroy);

	wl_list_insert(&output->layers[layer_surface->client_pending.layer], &cg_layer->link);
}

struct cg_layer_surface *
layer_from_wlr_layer_surface_v1(struct wlr_layer_surface_v1 *layer_surface)
{
	return layer_surface->data;
}