/*
 * Author: Leon.He
 * e-mail: 343005384@qq.com
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct buffer_object {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t handle;
	uint32_t size;
	uint32_t *vaddr;
	uint32_t fb_id;

    uint8_t r, g, b;
    bool r_up, g_up, b_up;
};

struct buffer_object buf[2];
struct buffer_object second_buf[2];

static int terminate;

static int modeset_create_fb(int fd, struct buffer_object *bo, uint32_t color)
{
	struct drm_mode_create_dumb create = {};
 	struct drm_mode_map_dumb map = {};
	uint32_t i;
    int ret = 0;

	create.width = bo->width;
	create.height = bo->height;
	create.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    if (ret != 0) {
        printf("create dump failed: %s\n", strerror(errno));
    }

	bo->pitch = create.pitch;
	bo->size = create.size;
	bo->handle = create.handle;
	ret = drmModeAddFB(fd, bo->width, bo->height, 24, 32, bo->pitch,
			   bo->handle, &bo->fb_id);
    if (ret != 0) {
        printf("drmModeAddFB failed: %s\n", strerror(errno));
    }

	map.handle = create.handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    if (ret != 0) {
        printf("DRM_IOCTL_MODE_MAP_DUMB failed: %s\n", strerror(errno));
    }

	bo->vaddr = mmap(0, create.size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, map.offset);
    if (bo->vaddr <= 0) {
        printf("mmap buf failed: %s\n", strerror(errno));
    }

    // draw
    srand(time(NULL));
	bo->r = rand() % 0xff;
	bo->g = rand() % 0xff;
	bo->b = rand() % 0xff;
    bo->r_up = bo->g_up = bo->b_up = true;
	for (i = 0; i < (bo->size / 4); i++)
		//bo->vaddr[i] = (bo->r<<16)|(bo->g<<8)|bo->b;
		bo->vaddr[i] = color;

	return 0;
}

static void modeset_destroy_fb(int fd, struct buffer_object *bo)
{
	struct drm_mode_destroy_dumb destroy = {};

	drmModeRmFB(fd, bo->fb_id);

	munmap(bo->vaddr, bo->size);

	destroy.handle = bo->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}


static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod)
{
	uint8_t next;

	next = cur + (*up ? 1 : -1) * (rand() % mod);
	if ((*up && next < cur) || (!*up && next > cur)) {
		*up = !*up;
		next = cur;
	}

	return next;
}

static void draw_region(struct buffer_object *buf, uint32_t top, uint32_t left,
                        uint32_t width, uint32_t height, int color)
{
    uint8_t *map = (uint8_t*)buf->vaddr;
	unsigned int j, k, off;
	for (j = top; j < top+height; ++j) {
		for (k = left; k < left+width; ++k) {
			off = buf->pitch * j + k * 4;
			*(uint32_t *)&map[off] = color;
		}
	}

}

static void draw_color_random(struct buffer_object *buf)
{
    bool r_up, g_up, b_up;

	r_up = g_up = b_up = true;

	buf->r = next_color(&buf->r_up, buf->r, 20);
	buf->g = next_color(&buf->g_up, buf->g, 10);
	buf->b = next_color(&buf->b_up, buf->b, 5);

    draw_region(buf, 0, 0, buf->width, buf->height, (buf->r<<16)|(buf->g<<8)|buf->b);
}

static uint32_t bleft = 0;
static uint32_t btop = 0;
static uint32_t bwidth = 100;
static uint32_t bheight = 100;
static uint32_t speedx = 2;
static uint32_t speedy = 2;

static void calc_next_position()
{
    if ((bleft + speedx + bwidth >= buf[0].width) || (bleft + speedx <= 0)) speedx = -speedx;
    if ((btop +  speedy + bheight >= buf[0].height) || (btop + speedy <= 0)) speedy = -speedy;
    bleft += speedx;
    btop += speedy;
}

static void draw_moving_box(struct buffer_object *buf)
{
    // draw bg
    draw_region(buf, 0, 0, buf->width, buf->height, 0xff0000);
    // draw box
    draw_region(buf, btop, bleft, bwidth, bheight, 0x0000ff);
    // pos of next frame 
    calc_next_position();
}

static void modeset_page_flip_handler(int fd, uint32_t frame,
				    uint32_t sec, uint32_t usec,
				    void *data)
{
	static int frontbuf = 1;
	uint32_t crtc_id = *(uint32_t *)data;
    int ret = 0;

    // first draw on front buffer
    if (1 > 2) draw_color_random(&buf[frontbuf^1]);
    if (4 > 2) draw_moving_box(&buf[frontbuf^1]);
    // then schedule page flip, buf[i] will be displayed at next page flip event
	ret = drmModePageFlip(fd, crtc_id, buf[frontbuf^1].fb_id,
	        DRM_MODE_PAGE_FLIP_EVENT, data);
    if (ret != 0) {
        printf("drmModePageFlip failed: %s\n", strerror(errno));
    }
    frontbuf ^= 1;
    printf("frame: %u, sec: %u, usec: %u\n", frame, sec, usec);
}


static void sigint_handler(int arg)
{
    printf("arg: %d\n", arg);
	terminate = 1;
}

int main(int argc, char **argv)
{
	int fd;
	drmEventContext ev = {};
	drmModeConnector *conn;
	drmModeRes *res;
    drmModePlaneRes *plane_res;
	uint32_t conn_id;
	uint32_t crtc_id;
    uint32_t plane_id;
    int ret = 0;
    int test_vsync = 0;
    if (argc == 2 && strcmp(argv[1], "vsync") == 0) {
        test_vsync = 1;
    }

	signal(SIGINT, sigint_handler);

	ev.version = DRM_EVENT_CONTEXT_VERSION;
	ev.page_flip_handler = modeset_page_flip_handler;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("open dri failed: %s\n", strerror(errno));
        return -1;
    }

	res = drmModeGetResources(fd);
    printf("crtc count: %d\n", res->count_crtcs);
	crtc_id = res->crtcs[0];
	conn_id = res->connectors[0];

    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret != 0) {
        printf("drmSetClientCap failed: %s\n", strerror(errno));
        return -1;
    }

    plane_res = drmModeGetPlaneResources(fd);
    printf("plane count: %d\n", plane_res->count_planes);
    plane_id = plane_res->planes[0];

	conn = drmModeGetConnector(fd, conn_id);
	buf[0].width = conn->modes[0].hdisplay;
	buf[0].height = conn->modes[0].vdisplay;
	buf[1].width = conn->modes[0].hdisplay;
	buf[1].height = conn->modes[0].vdisplay;
    second_buf[0].width = conn->modes[0].hdisplay;
	second_buf[0].height = conn->modes[0].vdisplay;
	second_buf[1].width = conn->modes[0].hdisplay;
	second_buf[1].height = conn->modes[0].vdisplay;

	modeset_create_fb(fd, &buf[0], 0xff0000);
	modeset_create_fb(fd, &buf[1], 0x0000ff);
    modeset_create_fb(fd, &second_buf[0], 0x00ff00);
	modeset_create_fb(fd, &second_buf[1], 0x00f000);



    if (test_vsync == 1) {
        // display buf[0]
        ret = drmModeSetCrtc(fd, crtc_id, buf[0].fb_id,
                0, 0, &conn_id, 1, &conn->modes[0]);
        if (ret < 0) {
            printf("drmModeSetCrtc failed: %s\n", strerror(errno));
            return -1;
        }
        
        // draw buf[1]
        draw_moving_box(&buf[1]);
        draw_color_random(&buf[1]);
        // buf[1] will be displayed at next page flip
        drmModePageFlip(fd, crtc_id, buf[1].fb_id,
                DRM_MODE_PAGE_FLIP_EVENT, &crtc_id);
        fd_set fds;
        FD_ZERO(&fds);
        while (1) {
    		FD_SET(0, &fds);
    		FD_SET(fd, &fds);
    		ret = select(fd + 1, &fds, NULL, NULL, NULL);
    		if (ret < 0) {
    			fprintf(stderr, "select() failed with %d: %m\n", errno);
    			break;
    		} else if (FD_ISSET(0, &fds)) {
    			fprintf(stderr, "exit due to user-input\n");
    			break;
    		} else if (FD_ISSET(fd, &fds)) {
    			drmHandleEvent(fd, &ev);
    		}
	    }
    } else {
        //bleft = 0;
        //btop = 1170;
        drmModeSetPlane(fd, plane_res->planes[0], crtc_id, buf[0].fb_id, 0,
                0, 1170, 1080, 1170,
                0, 0, 1080 << 16, 1170 << 16);
        getchar();
        drmModeSetPlane(fd, plane_res->planes[4], crtc_id, second_buf[0].fb_id, 0,
                0, 0, 1080, 1170,
                0, 0, 1080 << 16, 1170 << 16);
        getchar();
        getchar();
        int front0 = 0;
        int front1 = 0;
        while (!terminate) {
            draw_moving_box(&buf[front1^1]);
            drmModeSetPlane(fd, plane_res->planes[0], crtc_id, buf[front1^1].fb_id, 0,
                        0, 1170, 1080, 1170,
                        0, 1170, 1080 << 16, 1170 << 16);

            draw_color_random(&second_buf[front0^1]);
            drmModeSetPlane(fd, plane_res->planes[4], crtc_id, second_buf[front0^1].fb_id, 0,
                        0, 0, 1080, 1170,
                        0, 0, 1080 << 16, 1170 << 16);
            front0 ^= 1;
            front1 ^= 1;
            usleep(16000);
        }
    }


	modeset_destroy_fb(fd, &buf[1]);
	modeset_destroy_fb(fd, &buf[0]);

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);

	close(fd);

	return 0;
}
