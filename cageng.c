/*
 * Cage: A Wayland kiosk.
 *
 * Copyright (C) 2018-2020 Jente Hidskes
 *
 * See the LICENSE file accompanying this file.
 */

#define _POSIX_C_SOURCE 200112L

#include "config.h"

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "desktop/output.h"
#include "desktop/xdg_shell.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "serverng.h"

static int
sigchld_handler(int fd, uint32_t mask, void *user_data)
{
	struct wl_display *display = user_data;

	/* Close Cage's read pipe. */
	close(fd);

	if (mask & WL_EVENT_HANGUP) {
		wlr_log(WLR_DEBUG, "Child process closed normally");
	} else if (mask & WL_EVENT_ERROR) {
		wlr_log(WLR_DEBUG, "Connection closed by server");
	}

	wl_display_terminate(display);
	return 0;
}

static bool
set_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD);

	if (flags == -1) {
		wlr_log(WLR_ERROR, "Unable to set the CLOEXEC flag: fnctl failed");
		return false;
	}

	flags = flags | FD_CLOEXEC;
	if (fcntl(fd, F_SETFD, flags) == -1) {
		wlr_log(WLR_ERROR, "Unable to set the CLOEXEC flag: fnctl failed");
		return false;
	}

	return true;
}

static bool
spawn_primary_client(struct wl_display *display, char *argv[], pid_t *pid_out, struct wl_event_source **sigchld_source)
{
	int fd[2];
	if (pipe(fd) != 0) {
		wlr_log(WLR_ERROR, "Unable to create pipe");
		return false;
	}

	pid_t pid = fork();
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		/* Close read, we only need write in the primary client process. */
		close(fd[0]);
		execvp(argv[0], argv);
		_exit(1);
	} else if (pid == -1) {
		wlr_log_errno(WLR_ERROR, "Unable to fork");
		return false;
	}

	/* Set this early so that if we fail, the client process will be cleaned up properly. */
	*pid_out = pid;

	if (!set_cloexec(fd[0]) || !set_cloexec(fd[1])) {
		return false;
	}

	/* Close write, we only need read in Cage. */
	close(fd[1]);

	struct wl_event_loop *event_loop = wl_display_get_event_loop(display);
	uint32_t mask = WL_EVENT_HANGUP | WL_EVENT_ERROR;
	*sigchld_source = wl_event_loop_add_fd(event_loop, fd[0], mask, sigchld_handler, display);

	wlr_log(WLR_DEBUG, "Child process created with pid %d", pid);
	return true;
}

static void
cleanup_primary_client(pid_t pid)
{
	int status;

	waitpid(pid, &status, 0);

	if (WIFEXITED(status)) {
		wlr_log(WLR_DEBUG, "Child exited normally with exit status %d", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		wlr_log(WLR_DEBUG, "Child was terminated by a signal (%d)", WTERMSIG(status));
	}
}

static bool
drop_permissions(void)
{
	if (getuid() != geteuid() || getgid() != getegid()) {
		// Set the gid and uid in the correct order.
		if (setgid(getgid()) != 0 || setuid(getuid()) != 0) {
			wlr_log(WLR_ERROR, "Unable to drop root, refusing to start");
			return false;
		}
	}

	if (setgid(0) != -1 || setuid(0) != -1) {
		wlr_log(WLR_ERROR,
			"Unable to drop root (we shouldn't be able to restore it after setuid), refusing to start");
		return false;
	}

	return true;
}

static int
handle_signal(int signal, void *user_data)
{
	struct wl_display *display = user_data;

	switch (signal) {
	case SIGINT:
		/* Fallthrough */
	case SIGTERM:
		wl_display_terminate(display);
		return 0;
	default:
		return 0;
	}
}

static void
handle_view_unmapped(struct wl_listener *listener, void *user_data)
{
	// struct cg_server *server = wl_container_of(listener, server, view_unmapped);
	// struct cg_view *view = user_data;

	// no-op
}

static void
handle_view_mapped(struct wl_listener *listener, void *user_data)
{
	// struct cg_server *server = wl_container_of(listener, server, view_mapped);
	// struct cg_view *view = user_data;

	// no-op
}

static void
handle_xdg_shell_surface_new(struct wl_listener *listener, void *user_data)
{
	struct cg_server *server = wl_container_of(listener, server, new_xdg_shell_surface);
	struct wlr_xdg_surface *xdg_surface = user_data;

	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		return;
	}

	struct cg_xdg_shell_view *xdg_shell_view = calloc(1, sizeof(struct cg_xdg_shell_view));
	if (!xdg_shell_view) {
		wlr_log(WLR_ERROR, "Failed to allocate XDG Shell view");
		return;
	}

	struct cg_output *output = wl_container_of(server->outputs.next, output, link);
	cage_xdg_shell_view_init(xdg_shell_view, xdg_surface, output);

	server->view_mapped.notify = handle_view_mapped;
	wl_signal_add(&xdg_shell_view->view.events.map, &server->view_mapped);
	server->view_unmapped.notify = handle_view_unmapped;
	wl_signal_add(&xdg_shell_view->view.events.unmap, &server->view_unmapped);
}

static void
handle_new_output(struct wl_listener *listener, void *user_data)
{
	struct cg_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = user_data;

	struct cg_output *output = calloc(1, sizeof(struct cg_output));
	if (!output) {
		wlr_log(WLR_ERROR, "Failed to allocate output");
		return;
	}

	wlr_output_layout_add_auto(server->output_layout, wlr_output);

	// TODO: do this before or after init?
	wl_list_insert(&server->outputs, &output->link);
	cage_output_init(output, wlr_output);
}

static void
desktop_surface_at(struct cg_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy)
{
	struct wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, lx, ly);
	struct cg_output *output = wlr_output->data;

	cage_output_surface_at(output, lx, ly, surface, sx, sy);
}

static void
handle_cursor_motion(struct wl_listener *listener, void *user_data)
{
	struct cg_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_cursor *wlr_cursor = server->seat->cursor->wlr_cursor;
	struct wlr_seat *wlr_seat = server->seat->wlr_seat;
	uint32_t time = *(uint32_t *) user_data;

	double sx, sy;
	struct wlr_surface *surface = NULL;
	desktop_surface_at(server, wlr_cursor->x, wlr_cursor->y, &surface, &sx, &sy);

	if (!surface) {
		wlr_seat_pointer_clear_focus(wlr_seat);
	} else {
		wlr_seat_pointer_notify_enter(wlr_seat, surface, sx, sy);

		bool focus_changed = wlr_seat->pointer_state.focused_surface != surface;
		if (!focus_changed && time > 0) {
			wlr_seat_pointer_notify_motion(wlr_seat, time, sx, sy);
		}
	}
}

static void
handle_new_input(struct wl_listener *listener, void *user_data)
{
	struct cg_server *server = wl_container_of(listener, server, new_input);
	struct cg_seat *seat = server->seat;
	struct wlr_input_device *device = user_data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		cage_seat_add_new_keyboard(seat, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		cage_seat_add_new_pointer(seat, device);
		break;
	case WLR_INPUT_DEVICE_TOUCH:
		wlr_log(WLR_DEBUG, "Touch input is not implemented");
		break;
	case WLR_INPUT_DEVICE_SWITCH:
		wlr_log(WLR_DEBUG, "Switch input is not implemented");
		return;
	case WLR_INPUT_DEVICE_TABLET_TOOL:
	case WLR_INPUT_DEVICE_TABLET_PAD:
		wlr_log(WLR_DEBUG, "Tablet input is not implemented");
		return;
	}

	cage_seat_update_capabilities(seat);
}

static void
usage(FILE *file, const char *cage)
{
	fprintf(file,
		"Usage: %s [OPTIONS] [--] APPLICATION\n"
		"\n"
		" -h\t Display this help message\n"
		" -v\t Show the version number and exit\n"
		"\n"
		" Use -- when you want to pass arguments to APPLICATION\n",
		cage);
}

static bool
parse_args(struct cg_server *server, int argc, char *argv[])
{
	int c;
	while ((c = getopt(argc, argv, "hv")) != -1) {
		switch (c) {
		case 'h':
			usage(stdout, argv[0]);
			return false;
		case 'v':
			fprintf(stdout, "Cage version " CAGE_VERSION "\n");
			exit(0);
		default:
			usage(stderr, argv[0]);
			return false;
		}
	}

	if (optind >= argc) {
		usage(stderr, argv[0]);
		return false;
	}

	return true;
}

static struct cg_seat *
setup_seat(struct wl_display *wl_display, struct wlr_output_layout *output_layout)
{
	struct cg_seat *seat = calloc(1, sizeof(struct cg_seat));
	if (!seat) {
		wlr_log(WLR_ERROR, "Cannot allocate seat");
		goto fail;
	}

	struct wlr_seat *wlr_seat = wlr_seat_create(wl_display, "seat0");
	if (!wlr_seat) {
		wlr_log(WLR_ERROR, "Cannot allocate seat0");
		goto fail;
	}

	struct wlr_xcursor_manager *xcursor_manager = wlr_xcursor_manager_create(NULL, XCURSOR_SIZE);
	if (!xcursor_manager) {
		wlr_log(WLR_ERROR, "Cannot create XCursor manager");
		goto fail;
	}

	struct wlr_cursor *wlr_cursor = wlr_cursor_create();
	if (!wlr_cursor) {
		wlr_log(WLR_ERROR, "Unable to create wlr cursor");
		goto fail;
	}
	wlr_cursor_attach_output_layout(wlr_cursor, output_layout);

	struct cg_cursor *cursor = calloc(1, sizeof(struct cg_cursor));
	if (!cursor) {
		wlr_log(WLR_ERROR, "Cannot allocate cursor");
		goto fail;
	}

	cage_cursor_init(cursor, wlr_cursor, xcursor_manager, wlr_seat);
	cage_seat_init(seat, wlr_seat, cursor);
	return seat;

fail:
	free(cursor);
	if (wlr_cursor != NULL) {
		wlr_cursor_destroy(wlr_cursor);
	}
	wlr_xcursor_manager_destroy(xcursor_manager);
	wlr_seat_destroy(wlr_seat);
	free(seat);
	return NULL;
}

int
main(int argc, char *argv[])
{
	struct cg_server server = {0};
	struct wl_event_loop *event_loop = NULL;
	struct wl_event_source *sigint_source = NULL;
	struct wl_event_source *sigterm_source = NULL;
	struct wl_event_source *sigchld_source = NULL;
	struct wlr_backend *backend = NULL;
	struct wlr_renderer *renderer = NULL;
	struct wlr_compositor *compositor = NULL;
	struct wlr_xdg_shell *xdg_shell = NULL;
	struct wlr_data_device_manager *data_device_manager = NULL;
	pid_t pid = 0;
	int ret = 0;

	if (!parse_args(&server, argc, argv)) {
		return 1;
	}

#ifdef DEBUG
	wlr_log_init(WLR_DEBUG, NULL);
#else
	wlr_log_init(WLR_ERROR, NULL);
#endif

	/* Wayland requires XDG_RUNTIME_DIR to be set. */
	if (!getenv("XDG_RUNTIME_DIR")) {
		wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR is not set in the environment");
		return 1;
	}

	server.wl_display = wl_display_create();
	if (!server.wl_display) {
		wlr_log(WLR_ERROR, "Cannot allocate a Wayland display");
		return 1;
	}

	event_loop = wl_display_get_event_loop(server.wl_display);
	sigint_source = wl_event_loop_add_signal(event_loop, SIGINT, handle_signal, &server.wl_display);
	sigterm_source = wl_event_loop_add_signal(event_loop, SIGTERM, handle_signal, &server.wl_display);

	backend = wlr_backend_autocreate(server.wl_display, NULL);
	if (!backend) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots backend");
		ret = 1;
		goto end;
	}

	if (!drop_permissions()) {
		ret = 1;
		goto end;
	}

	renderer = wlr_backend_get_renderer(backend);
	wlr_renderer_init_wl_display(renderer, server.wl_display);

	compositor = wlr_compositor_create(server.wl_display, renderer);
	if (!compositor) {
		wlr_log(WLR_ERROR, "Unable to create the wlroots compositor");
		ret = 1;
		goto end;
	}

	server.output_layout = wlr_output_layout_create();
	if (!server.output_layout) {
		wlr_log(WLR_ERROR, "Unable to create output layout");
		ret = 1;
		goto end;
	}

	data_device_manager = wlr_data_device_manager_create(server.wl_display);
	if (!data_device_manager) {
		wlr_log(WLR_ERROR, "Unable to create the data device manager");
		ret = 1;
		goto end;
	}

	wl_list_init(&server.outputs);
	server.new_output.notify = handle_new_output;
	wl_signal_add(&backend->events.new_output, &server.new_output);

	xdg_shell = wlr_xdg_shell_create(server.wl_display);
	if (!xdg_shell) {
		wlr_log(WLR_ERROR, "Unable to create the XDG shell interface");
		ret = 1;
		goto end;
	}
	server.new_xdg_shell_surface.notify = handle_xdg_shell_surface_new;
	wl_signal_add(&xdg_shell->events.new_surface, &server.new_xdg_shell_surface);

	struct cg_seat *seat = setup_seat(server.wl_display, server.output_layout);
	if (!seat) {
		ret = 1;
		goto end;
	}
	server.seat = seat;
	server.new_input.notify = handle_new_input;
	wl_signal_add(&backend->events.new_input, &server.new_input);
	server.cursor_motion.notify = handle_cursor_motion;
	wl_signal_add(&seat->cursor->events.motion, &server.cursor_motion);

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_log_errno(WLR_ERROR, "Unable to open Wayland socket");
		ret = 1;
		goto end;
	}

	if (!wlr_backend_start(backend)) {
		wlr_log(WLR_ERROR, "Unable to start the wlroots backend");
		ret = 1;
		goto end;
	}

	if (setenv("WAYLAND_DISPLAY", socket, true) < 0) {
		wlr_log_errno(WLR_ERROR, "Unable to set WAYLAND_DISPLAY. Clients may not be able to connect");
	} else {
		wlr_log(WLR_DEBUG, "Cage " CAGE_VERSION " is running on Wayland display %s", socket);
	}

	if (!spawn_primary_client(server.wl_display, argv + optind, &pid, &sigchld_source)) {
		ret = 1;
		goto end;
	}

	wl_display_run(server.wl_display);
	wl_display_destroy_clients(server.wl_display);

end:
	cleanup_primary_client(pid);

	wl_event_source_remove(sigint_source);
	wl_event_source_remove(sigterm_source);
	if (sigchld_source) {
		wl_event_source_remove(sigchld_source);
	}
	cage_seat_fini(server.seat);
	/* This function is not null-safe, but we only ever get here
	   with a proper wl_display. */
	wl_display_destroy(server.wl_display);
	wlr_output_layout_destroy(server.output_layout);
	return ret;
}
