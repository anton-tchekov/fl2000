#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#include "../include/fl2000_ioctl.h"

#define FL2K_NAME                    "/dev/fl2000-0"

#define IMAGE_ASPECT_RATIO_16_10    0
#define IMAGE_ASPECT_RATIO_4_3      1
#define IMAGE_ASPECT_RATIO_5_4      2
#define IMAGE_ASPECT_RATIO_16_9     3

#define MAX_FRAME_BUFFER_SIZE        (1920 * 1080 * 3)
#define NUM_FRAME_BUFFERS          16

#define	USE_COMPRESSION             0
#define	COMPRESS_SIZE_LIMIT         0

#define	OUTPUT_COLOR_FORMAT          COLOR_FORMAT_RGB_24

struct resolution
{
	uint32_t width, height;
};

static struct monitor_info monitor_info;
static uint8_t frame_buffers[NUM_FRAME_BUFFERS][MAX_FRAME_BUFFER_SIZE];

static int kbhit(void)
{
	struct termios oldt, newt;
	int ch;
	int oldf;
	tcgetattr(STDIN_FILENO, &oldt);
	newt = oldt;
	newt.c_lflag &= ~(ICANON | ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
	fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
	ch = getchar();
	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
	fcntl(STDIN_FILENO, F_SETFL, oldf);
	if(ch != EOF)
	{
		ungetc(ch, stdin);
		return 1;
	}

	return 0;
}

static void init_frame_by_test_pattern(
	uint8_t *frame_buffer,
	uint32_t width,
	uint32_t height)
{
	static int q = 0;

	memset(frame_buffer, 0, 3 * width * height);

	uint8_t n = 0;
	for(uint32_t i = 0; i < 3 * width * height; i += 3)
	{
		frame_buffer[i + 0] = q ? n : 0;
		frame_buffer[i + 1] = q ? 0 : n;
		frame_buffer[i + 2] = 0xFF;

		++n;
	}

	q = !q;
}

static void test_display(int fd, uint32_t width, uint32_t height)
{
	struct display_mode display_mode;
	struct surface_info surface_info;
	struct surface_update_info update_info;
	uint8_t *frame_buffer;
	int ret_val;
	memset(&surface_info, 0, sizeof(surface_info));

	frame_buffer = frame_buffers[0];
	surface_info.handle = (unsigned long)frame_buffer;
	surface_info.user_buffer = (unsigned long)frame_buffer;

	init_frame_by_test_pattern(frame_buffer, width, height);

	surface_info.buffer_length = width * height * 3;
	surface_info.width = width;
	surface_info.height = height;
	surface_info.pitch = width * 3;
	surface_info.color_format = COLOR_FORMAT_RGB_24;
	surface_info.type = SURFACE_TYPE_VIRTUAL_FRAGMENTED_VOLATILE;
	fprintf(stderr, "create_surface(%u, %u) , type(0x%x)\n",
			width, height, surface_info.type);

	ret_val = ioctl(fd, IOCTL_FL2000_CREATE_SURFACE, &surface_info);
	if(ret_val < 0)
	{
		fprintf(stderr, "IOCTL_FL2000_CREATE_SURFACE failed %d\n", ret_val);
		return;
	}

	memset(&display_mode, 0, sizeof(display_mode));
	display_mode.width = width;
	display_mode.height = height;
	display_mode.refresh_rate = 60;
	display_mode.input_color_format = COLOR_FORMAT_RGB_24;
	display_mode.output_color_format = OUTPUT_COLOR_FORMAT;
	display_mode.use_compression = USE_COMPRESSION;
	display_mode.compress_size_limit = COMPRESS_SIZE_LIMIT;

	ret_val = ioctl(fd, IOCTL_FL2000_SET_DISPLAY_MODE, &display_mode);
	if(ret_val < 0)
	{
		fprintf(stderr, "IOCTL_FL2000_SET_DISPLAY_MODE failed %d\n", ret_val);
		return;
	}

	memset(&update_info, 0, sizeof(update_info));
	update_info.handle = surface_info.handle;
	update_info.user_buffer = surface_info.user_buffer;
	update_info.buffer_length = width * height * 3;

	ret_val = ioctl(fd, IOCTL_FL2000_NOTIFY_SURFACE_UPDATE, &update_info);
	if(ret_val < 0)
	{
		printf("here! nooo!\n");
		fprintf(stderr, "IOCTL_FL2000_NOTIFY_SURFACE_UPDATE failed %d\n", ret_val);
		return;
	}

	/* Close */
	fprintf(stdout, "display_mode(%u, %u), press any key to continue\n", width, height);

	Display *display = XOpenDisplay(NULL);
	Window root = DefaultRootWindow(display);
	XWindowAttributes attributes = { 0 };
	XGetWindowAttributes(display, root, &attributes);

	while(kbhit() == 0)
	{
		Window window_returned;
		unsigned int mask_return;
		int root_x, root_y, win_x, win_y;
		XQueryPointer(display, root, &window_returned,
			&window_returned, &root_x, &root_y, &win_x, &win_y,
			&mask_return);

		root_x -= 3520;

		XImage *img = XGetImage(display, root, 3520, 0, 1680, 1050, AllPlanes, ZPixmap);

		for(int y = 0; y < 1050; ++y)
		{
			for(int x = 0; x < 1680; ++x)
			{
				uint32_t in = XGetPixel(img, x, y);
				uint8_t *out = frame_buffer + 3 * (y * width + x);

				out[0] = in & 0xFF;
				out[1] = (in >> 8) & 0xFF;
				out[2] = (in >> 16) & 0xFF;

				if(x >= root_x - 5 && x <= root_x + 5 && y >= root_y - 5 && y <= root_y + 5)
				{
					out[0] = 0;
					out[1] = 0;
					out[2] = 0xff;
				}
			}
		}

		XDestroyImage(img);

		{
			memset(&update_info, 0, sizeof(update_info));
			update_info.handle = surface_info.handle;
			update_info.user_buffer = surface_info.user_buffer;
			update_info.buffer_length = width * height * 3;

			ret_val = ioctl(fd, IOCTL_FL2000_NOTIFY_SURFACE_UPDATE, &update_info);
			if(ret_val < 0)
			{
				fprintf(stderr, "IOCTL_FL2000_NOTIFY_SURFACE_UPDATE failed %d\n", ret_val);
				return;
			}
		}

	}

	XCloseDisplay(display);


	getchar();

	memset(&display_mode, 0, sizeof(display_mode));
	ret_val = ioctl(fd, IOCTL_FL2000_SET_DISPLAY_MODE, &display_mode);
	ret_val = ioctl(fd, IOCTL_FL2000_DESTROY_SURFACE, &surface_info);
	if(ret_val < 0)
	{
		fprintf(stderr, "IOCTL_FL2000_DESTROY_SURFACE failed %d\n", ret_val);
		return;
	}
}

int main(int argc, char *argv[])
{
	int fd, ioctl_ret;
	int width, height;

	/* Check for root */
	if(geteuid() != 0)
	{
		fprintf(stderr, "Root required, try again with sudo\n");
		return 1;
	}

	/* Check parameters */
	if(argc != 3)
	{
		fprintf(stderr, "Usage: ./fltest width height\n");
		return 1;
	}

	/* Connect to FL2000 */
	fd = open(FL2K_NAME, O_RDWR);
	if(fd == -1)
	{
		fprintf(stderr, "FL2000 device not connected\n");
		return 1;
	}

	ioctl_ret = ioctl(fd, IOCTL_FL2000_QUERY_MONITOR_INFO, &monitor_info);
	if(ioctl_ret < 0)
	{
		fprintf(stderr, "IOCTL_FL2000_QUERY_MONITOR_INFO failed: %d\n", ioctl_ret);
		goto exit;
	}

	if(monitor_info.monitor_flags.connected == 0)
	{
		fprintf(stderr, "No monitor connected to FL2000\n");
		goto exit;
	}

	/* Initialize */
	width = atoi(argv[1]);
	height = atoi(argv[2]);
	if(3 * width * height > MAX_FRAME_BUFFER_SIZE)
	{
		fprintf(stderr, "Resolution too large\n");
		goto exit;
	}

	test_display(fd, width, height);

exit:
	close(fd);
	return 0;
}
