/*
	libwpe backend for the EGL drivers of set-top boxes, which render
	straight to the display: the Mali-450 fbdev driver into /dev/fb0, the
	Broadcom V3D driver (HBBTV_NXPL) into a Nexus graphics window above the
	video. There is no compositor in between.

	Where the display is Neutrino's own GL window (HBBTV_GBM), the pages
	are rendered into GBM buffers that Neutrino gets as dmabufs and shows
	in place of its OSD; libstb-hal's glfb.cpp has the messages.

	License: GPL

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public
	License as published by the Free Software Foundation; either
	version 2 of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <EGL/egl.h>
#include <fcntl.h>
#include <glib.h>
#ifdef HBBTV_GBM
#include <errno.h>
#include <gbm.h>
#include <glib-unix.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#endif
#include <linux/fb.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wpe/wpe-egl.h>
#include <wpe/wpe.h>

#ifdef HBBTV_NXPL
/* the Nexus platform API of the Broadcom V3D driver, which has no header */
typedef void *NXPL_PlatformHandle;
typedef struct {
	int a, b;
	bool subtract_cd;
	int c, d;
	bool subtract_e;
	int e;
} NEXUS_BlendEquation;
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	bool stretch;
	uint32_t clientID;
	uint32_t zOrder;
	NEXUS_BlendEquation colorBlend;
	NEXUS_BlendEquation alphaBlend;
	uint32_t magic;
} NXPL_NativeWindowInfoEXT;
int NEXUS_Platform_AuthenticatedJoin(const void *settings);
void NXPL_RegisterNexusDisplayPlatform(NXPL_PlatformHandle *handle, void *display);
void NXPL_UnregisterNexusDisplayPlatform(NXPL_PlatformHandle handle);
void NXPL_GetDefaultNativeWindowInfoEXT(NXPL_NativeWindowInfoEXT *info);
void *NXPL_CreateNativeWindowEXT(const NXPL_NativeWindowInfoEXT *info);
void NXPL_DestroyNativeWindow(void *window);
#else
/* the window type of the Mali fbdev EGL platform */
struct fbdev_window {
	unsigned short width;
	unsigned short height;
};
#endif

#if !defined(HBBTV_NXPL) && !defined(HBBTV_GBM)
static void fb_size(uint32_t *width, uint32_t *height)
{
	struct fb_var_screeninfo var;
	int fd = open("/dev/fb0", O_RDONLY | O_CLOEXEC);

	*width = 1280;
	*height = 720;
	if (fd < 0)
		return;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 && var.xres && var.yres) {
		*width = var.xres;
		*height = var.yres;
	}
	close(fd);
}
#endif

/* ui process: renderer host */

static void *host_create(void)
{
	return NULL;
}

static void host_destroy(void *data)
{
	(void)data;
}

/* the web process does not talk back through this socket, it only has to exist */
static int host_create_client(void *data)
{
	int fds[2];

	(void)data;
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0)
		return -1;
	close(fds[0]);
	return fds[1];
}

static const struct wpe_renderer_host_interface host_interface = {
	.create = host_create,
	.destroy = host_destroy,
	.create_client = host_create_client,
};

/* ui process: view backend */

struct view {
	struct wpe_view_backend *backend;
	int fds[2];
};

static void *view_create(void *params, struct wpe_view_backend *backend)
{
	struct view *view = g_new0(struct view, 1);

	(void)params;
	view->backend = backend;
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, view->fds) < 0)
		view->fds[0] = view->fds[1] = -1;
	return view;
}

static void view_destroy(void *data)
{
	struct view *view = data;

	if (view->fds[0] >= 0)
		close(view->fds[0]);
	if (view->fds[1] >= 0)
		close(view->fds[1]);
	g_free(view);
}

/* HbbTV applications lay out for 1280x720; the framebuffer gets the
 * rendering at its own resolution through the device scale factor, the
 * Nexus window is stretched */
static void view_initialize(void *data)
{
	struct view *view = data;
	uint32_t width = 1280, height = 720;

#if !defined(HBBTV_NXPL) && !defined(HBBTV_GBM)
	fb_size(&width, &height);
#endif
	float scale = height / 720.0f;
	wpe_view_backend_dispatch_set_device_scale_factor(view->backend, scale);
	wpe_view_backend_dispatch_set_size(view->backend, width / scale, height / scale);
}

/* with GBM the renderer gets the socket to Neutrino's display */
static int view_get_renderer_host_fd(void *data)
{
	struct view *view = data;

#ifdef HBBTV_GBM
	const char *display = g_getenv("NEUTRINO_HBBTV_DISPLAY");

	if (display)
		return dup(atoi(display));
#endif
	return dup(view->fds[1]);
}

static const struct wpe_view_backend_interface view_interface = {
	.create = view_create,
	.destroy = view_destroy,
	.initialize = view_initialize,
	.get_renderer_host_fd = view_get_renderer_host_fd,
};

/* web process: renderer backend */

#ifdef HBBTV_NXPL
static NXPL_PlatformHandle nxpl;
#endif

#ifdef HBBTV_GBM
/* the GPU by its render node, which needs no rights on the display */
struct egl {
	int fd;
	struct gbm_device *gbm;
};

static gboolean open_gpu(struct egl *egl, const char *path)
{
	egl->fd = open(path, O_RDWR | O_CLOEXEC);
	if (egl->fd < 0)
		return FALSE;
	egl->gbm = gbm_create_device(egl->fd);
	if (!egl->gbm) {
		close(egl->fd);
		egl->fd = -1;
		return FALSE;
	}
	if (g_getenv("NEUTRINO_HBBTV_DEBUG"))
		fprintf(stderr, "neutrino-hbbtv: rendering on %s\n", path);
	return TRUE;
}

static void *egl_create(int host_fd)
{
	struct egl *egl = g_new0(struct egl, 1);

	if (host_fd >= 0)
		close(host_fd);
	egl->fd = -1;
	/* the GPU Neutrino draws with first: a dmabuf of another one, or of a
	 * device without a GPU such as vgem, may not be shown */
	const char *node = g_getenv("NEUTRINO_HBBTV_RENDER_NODE");
	if (!(node && *node && open_gpu(egl, node))) {
		for (int i = 128; i < 136; i++) {
			char path[32];

			snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
			if (open_gpu(egl, path))
				break;
		}
	}
	if (!egl->gbm)
		g_warning("no GPU to render with");
	return egl;
}

static void egl_destroy(void *data)
{
	struct egl *egl = data;

	if (egl->gbm) {
		gbm_device_destroy(egl->gbm);
		close(egl->fd);
	}
	g_free(egl);
}

static EGLNativeDisplayType egl_get_native_display(void *data)
{
	struct egl *egl = data;

	return (EGLNativeDisplayType)egl->gbm;
}

static uint32_t egl_get_platform(void *data)
{
	(void)data;
	return 0x31D7; /* EGL_PLATFORM_GBM_KHR */
}
#else
static void *egl_create(int host_fd)
{
	if (host_fd >= 0)
		close(host_fd);
#ifdef HBBTV_NXPL
	NEXUS_Platform_AuthenticatedJoin(NULL);
	NXPL_RegisterNexusDisplayPlatform(&nxpl, NULL);
#endif
	return g_new0(int, 1);
}

static void egl_destroy(void *data)
{
#ifdef HBBTV_NXPL
	NXPL_UnregisterNexusDisplayPlatform(nxpl);
#endif
	g_free(data);
}

static EGLNativeDisplayType egl_get_native_display(void *data)
{
	(void)data;
	return EGL_DEFAULT_DISPLAY;
}

static uint32_t egl_get_platform(void *data)
{
	(void)data;
	return 0;
}
#endif

static const struct wpe_renderer_backend_egl_interface egl_interface = {
	.create = egl_create,
	.destroy = egl_destroy,
	.get_native_display = egl_get_native_display,
	.get_platform = egl_get_platform,
};

/* web process: the window on the framebuffer, or the buffers for Neutrino */

struct target {
	struct wpe_renderer_backend_egl_target *target;
#ifdef HBBTV_NXPL
	void *window;
#elif defined(HBBTV_GBM)
	struct gbm_surface *surface;
	int display;		/* the socket to Neutrino */
	struct gbm_bo *sent;	/* the picture Neutrino has not confirmed yet */
	struct gbm_bo *shown;	/* the one it shows */
	unsigned buffers;	/* how many it knows */
#else
	struct fbdev_window window;
#endif
	GSource *frame_source;
};

static void *target_create(struct wpe_renderer_backend_egl_target *wpe_target, int host_fd)
{
	struct target *target = g_new0(struct target, 1);

#ifdef HBBTV_GBM
	target->display = host_fd;
#else
	if (host_fd >= 0)
		close(host_fd);
#endif
	target->target = wpe_target;
	return target;
}

static void target_destroy(void *data)
{
	struct target *target = data;

	if (target->frame_source) {
		g_source_destroy(target->frame_source);
		g_source_unref(target->frame_source);
	}
#ifdef HBBTV_GBM
	if (target->display >= 0)
		close(target->display);
#endif
	g_free(target);
}

#ifdef HBBTV_NXPL
/* the window is stretched over the display, above the video plane */
static void target_initialize(void *data, void *backend, uint32_t width, uint32_t height)
{
	struct target *target = data;
	NXPL_NativeWindowInfoEXT info;

	(void)backend;
	NXPL_GetDefaultNativeWindowInfoEXT(&info);
	info.width = width;
	info.height = height;
	info.stretch = true;
	info.zOrder = 1;
	target->window = NXPL_CreateNativeWindowEXT(&info);
}

static EGLNativeWindowType target_get_native_window(void *data)
{
	struct target *target = data;

	return (EGLNativeWindowType)target->window;
}

static void target_resize(void *data, uint32_t width, uint32_t height)
{
	(void)data;
	(void)width;
	(void)height;
}
#elif defined(HBBTV_GBM)
static void target_initialize(void *data, void *backend, uint32_t width, uint32_t height)
{
	struct target *target = data;
	struct egl *egl = backend;

	if (egl->gbm)
		target->surface = gbm_surface_create(egl->gbm, width, height, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
	if (!target->surface)
		g_warning("no surface of %ux%u to render into", width, height);
}

static EGLNativeWindowType target_get_native_window(void *data)
{
	struct target *target = data;

	return (EGLNativeWindowType)target->surface;
}

/* the pages are laid out for one size, which Neutrino stretches */
static void target_resize(void *data, uint32_t width, uint32_t height)
{
	(void)data;
	(void)width;
	(void)height;
}

/* what Neutrino knows a buffer by, and its dmabuf */
struct buffer {
	unsigned id;
	int fd;
};

static void buffer_destroy(struct gbm_bo *bo, void *data)
{
	struct buffer *buffer = data;

	(void)bo;
	if (buffer->fd >= 0)
		close(buffer->fd);
	g_free(buffer);
}

static void display_send(struct target *target, const char *text, int fd)
{
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} control;
	struct iovec iov = { (void *)text, strlen(text) };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };

	if (fd >= 0) {
		struct cmsghdr *cmsg;

		msg.msg_control = control.buf;
		msg.msg_controllen = sizeof(control.buf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	}
	if (sendmsg(target->display, &msg, MSG_NOSIGNAL) < 0)
		g_warning("display: %s", g_strerror(errno));
}

/* "done <id>": the picture sent last is on the display, the one before it
 * is free again and the next may be rendered */
static gboolean display_ready(gint fd, GIOCondition condition, gpointer data)
{
	struct target *target = data;
	char text[32];
	ssize_t n = recv(fd, text, sizeof(text) - 1, MSG_DONTWAIT);

	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR) && !(condition & G_IO_HUP))
			return G_SOURCE_CONTINUE;
		g_source_unref(target->frame_source);
		target->frame_source = NULL;
		return G_SOURCE_REMOVE;
	}
	if (!target->sent)
		return G_SOURCE_CONTINUE;
	if (target->shown)
		gbm_surface_release_buffer(target->surface, target->shown);
	target->shown = target->sent;
	target->sent = NULL;
	wpe_renderer_backend_egl_target_dispatch_frame_complete(target->target);
	return G_SOURCE_CONTINUE;
}
#else
static void target_initialize(void *data, void *backend, uint32_t width, uint32_t height)
{
	struct target *target = data;

	(void)backend;
	target->window.width = width;
	target->window.height = height;
}

static EGLNativeWindowType target_get_native_window(void *data)
{
	struct target *target = data;

	return (EGLNativeWindowType)&target->window;
}

static void target_resize(void *data, uint32_t width, uint32_t height)
{
	struct target *target = data;

	target->window.width = width;
	target->window.height = height;
}
#endif

static void target_frame_will_render(void *data)
{
	(void)data;
}

#ifndef HBBTV_GBM
/* the swap has already waited for the display; report the frame from the
 * compositor's own loop so WebKit does not re-enter it from frame_rendered */
static gboolean frame_complete(gpointer data)
{
	struct target *target = data;

	g_source_unref(target->frame_source);
	target->frame_source = NULL;
	wpe_renderer_backend_egl_target_dispatch_frame_complete(target->target);
	return G_SOURCE_REMOVE;
}
#endif

#ifdef HBBTV_GBM
/* the picture goes to Neutrino, whose answer completes the frame; the
 * answers are read in the compositor's own loop */
static void target_frame_rendered(void *data)
{
	struct target *target = data;
	struct gbm_bo *bo = target->surface ? gbm_surface_lock_front_buffer(target->surface) : NULL;
	char text[96];

	if (!bo || target->display < 0) {
		g_warning("nothing to show the page with");
		return;
	}
	if (!target->frame_source) {
		target->frame_source = g_unix_fd_source_new(target->display, G_IO_IN | G_IO_HUP);
		g_source_set_callback(target->frame_source, G_SOURCE_FUNC(display_ready), target, NULL);
		g_source_attach(target->frame_source, g_main_context_get_thread_default());
	}
	if (target->sent)
		gbm_surface_release_buffer(target->surface, target->sent);
	target->sent = bo;

	struct buffer *buffer = gbm_bo_get_user_data(bo);
	if (!buffer) {
		buffer = g_new0(struct buffer, 1);
		buffer->id = ++target->buffers;
		buffer->fd = gbm_bo_get_fd(bo);
		gbm_bo_set_user_data(bo, buffer, buffer_destroy);
		snprintf(text, sizeof(text), "buffer %u %u %u %u %u %llu", buffer->id, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
			 gbm_bo_get_stride(bo), gbm_bo_get_format(bo), (unsigned long long)gbm_bo_get_modifier(bo));
		display_send(target, text, buffer->fd);
	}
	/* the swap only queued the rendering; a dmabuf is readable once the
	 * GPU is done with it, and Neutrino is to get the finished picture */
	struct pollfd done = { .fd = buffer->fd, .events = POLLIN };
	poll(&done, 1, 500);
	snprintf(text, sizeof(text), "frame %u", buffer->id);
	display_send(target, text, -1);
}
#else
static void target_frame_rendered(void *data)
{
	struct target *target = data;

	if (target->frame_source)
		return;
	target->frame_source = g_idle_source_new();
	g_source_set_priority(target->frame_source, G_PRIORITY_HIGH);
	g_source_set_callback(target->frame_source, frame_complete, target, NULL);
	g_source_attach(target->frame_source, g_main_context_get_thread_default());
}
#endif

static void target_deinitialize(void *data)
{
#ifdef HBBTV_NXPL
	struct target *target = data;

	if (target->window)
		NXPL_DestroyNativeWindow(target->window);
	target->window = NULL;
#elif defined(HBBTV_GBM)
	struct target *target = data;

	if (target->surface)
		gbm_surface_destroy(target->surface);
	target->surface = NULL;
	target->sent = target->shown = NULL;
#else
	(void)data;
#endif
}

static const struct wpe_renderer_backend_egl_target_interface target_interface = {
	.create = target_create,
	.destroy = target_destroy,
	.initialize = target_initialize,
	.get_native_window = target_get_native_window,
	.resize = target_resize,
	.frame_will_render = target_frame_will_render,
	.frame_rendered = target_frame_rendered,
	.deinitialize = target_deinitialize,
};

/* offscreen contexts fall back to pbuffers */

static void *offscreen_create(void)
{
	return g_new0(int, 1);
}

static void offscreen_destroy(void *data)
{
	g_free(data);
}

static void offscreen_initialize(void *data, void *backend)
{
	(void)data;
	(void)backend;
}

static EGLNativeWindowType offscreen_get_native_window(void *data)
{
	(void)data;
	return (EGLNativeWindowType)0;
}

static const struct wpe_renderer_backend_egl_offscreen_target_interface offscreen_interface = {
	.create = offscreen_create,
	.destroy = offscreen_destroy,
	.initialize = offscreen_initialize,
	.get_native_window = offscreen_get_native_window,
};

static void *load_object(const char *name)
{
	if (g_str_equal(name, "_wpe_renderer_host_interface"))
		return (void *)&host_interface;
	if (g_str_equal(name, "_wpe_view_backend_interface"))
		return (void *)&view_interface;
	if (g_str_equal(name, "_wpe_renderer_backend_egl_interface"))
		return (void *)&egl_interface;
	if (g_str_equal(name, "_wpe_renderer_backend_egl_target_interface"))
		return (void *)&target_interface;
	if (g_str_equal(name, "_wpe_renderer_backend_egl_offscreen_target_interface"))
		return (void *)&offscreen_interface;
	return NULL;
}

__attribute__((visibility("default")))
struct wpe_loader_interface _wpe_loader_interface = {
	.load_object = load_object,
};
