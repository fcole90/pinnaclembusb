/* Pinnacle Moviebox USB playback program
 * (C) 2006 Jonathan Campbell Impact Studio Pro
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <usb.h>

#include "libpmb.h"

static unsigned char buffer[2048];
int main(int argc,char **argv)
{
	int src_fd = open(argv[1],O_RDONLY);
	if (src_fd < 0) return 1;
	int s,f=0;

	// initialize libusb
	usb_init();
	usb_find_busses();
	usb_find_devices();

	if (PinnacleMovieBoxInit() < 0) {
		fprintf(stderr,"Cannot initialize Pinnacle MovieBox device\n");
		return 1;
	}

	do {
		while ((s = read(src_fd,buffer,2048)) > 0) {
			PinnacleMovieBoxWriteVideo(buffer,s);
		}
	} while (1);

	PinnacleMovieBoxFree();
}

