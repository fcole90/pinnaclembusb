/* Pinnacle Moviebox USB playback daemon
 * (C) 2006 Jonathan Campbell Impact Studio Pro
 *
 * Since the current code only knows how to initialize and cannot
 * re-initialize per MPEG file, we run as a daemon that is fed MPEG
 * via a FIFO from other programs. We also accept commands from another
 * FIFO.
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <syslog.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <usb.h>

#include "libpmb.h"

// the MovieBox does not handle SCR resets very well.
// if you play an MPEG file into it and then play another without filtering
// and the other MPEG also starts with a SCR of 0, the MovieBox will stall,
// waiting until it's internal clock matches (which won't in a LONG time).
// 
// Now the purpose of this daemon is to accept arbitrary MPEGs through a FIFO.
// That can easily happen, so we must massage the SCR, PTS, and DTS timestamps
// in the stream before sending it on to the Pinnacle MovieBox.
static unsigned long long monotonic_SCR = 0;
static char *pipename,*cmdpipe;
static int sigpipe = 0;
static int die = 0;

void sigma(int x)
{
	if (x == SIGPIPE) {
		sigpipe++;
	}
	else if (x == SIGTERM || x == SIGQUIT || x == SIGINT) {
		die = 1;
	}
}

static int mpeg_state = 0;
static unsigned long mpeg_sync = 0;
static unsigned char mpeg_in[4096];
static int mpeg_in_remain = 0;
static unsigned char mpeg_out[4096];
static int mpeg_outi = 0;
static unsigned long long last_SCR = 0,last_SCR_delta = 0,last_SCR_difference = 0;
static int warn_nonmpa = 0;

//static int debug_fd = -1;
void FlushMPEGOut()
{
	if (mpeg_outi <= 0)
		return;

//	if (debug_fd < 0) {
//		debug_fd = open("/tmp/test.mpg",O_WRONLY|O_CREAT|O_TRUNC,0644);
//		if (debug_fd < 0) {
//			fprintf(stderr,"Cannot create test MPEG %s\n",strerror(errno));
//		}
//	}

	if (mpeg_outi < 2048) {
		fprintf(stderr,"Packet too short\n");
		return;
	}
	else if (mpeg_outi > 2048) {
		fprintf(stderr,"Packet too long\n");
		return;
	}

//	if (debug_fd >= 0)
//		write(debug_fd,mpeg_out,2048);

	PinnacleMovieBoxWriteVideo(mpeg_out,2048);
	mpeg_outi = 0;
}

static int AlreadyB3=0;
void StripThings(unsigned char *buf,unsigned char *fence)
{
	while (buf < (fence-3)) {
		// allow only ONE occurence of Sequence Header. MovieBox stalls on second occurence

		// remove "end of sequence code"
		if (	buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01 &&
			(buf[3] == 0xB7 || buf[3] == 0xB8 || buf[3] == 0xB9)) {
			fprintf(stderr,"Filtered out 0x000001%02X (AlreadyB3=%d)\n",buf[3],AlreadyB3);
			buf[0] = buf[1] = buf[2] = buf[3] = 0x00;
		}

		buf++;
	}
}

// here, buf points directly after the syncword.
int CheckModPacket(unsigned char *buf,int len,int syncword,int *skipped)
{
	int pkt_len = (((int)buf[0]) << 8) | ((int)buf[1]);
	if ((pkt_len+2) > len) {
		fprintf(stderr,"CheckModPacket: Packet too long for input buffer (%u bytes > %u), rejecting\n",
			pkt_len+2,len);
		return 0;
	}

	unsigned char *fence = buf + 2 + pkt_len;
	unsigned char *hdr = buf + 2;
	unsigned char *payload = NULL;

	// pick through the various packet header flags
	if ((*hdr >> 6) == 2) {	// MPEG-2 '10'
		unsigned char b;

		b = *hdr++;
		// '10'						bits[7...6]
		// PES_scrambling_control			bits[5...4]
		// PES_priority					bits[3]
		// data_alignment_indicator			bits[2]
		// copyright					bits[1]
		// original_or_copy				bits[0]
		b = *hdr++;
		unsigned char PTS_DTS_flags =			b >> 6;
		unsigned char ESCR_flag =			(b >> 5) & 1;
		// ES_rate_flag					bits[4]
		// DSM_trick_mode_flag				bits[3]
		// additional_copy_info_flag			bits[2]
		// PES_CRC_flag					bits[1]
		// PES_extension_flag				bits[0]
		unsigned char PES_header_data_length = *hdr++;

		// yay, we know where the payload is now
		payload = hdr + PES_header_data_length;

		// decode and modify the PTS/DTS/ESCR timestamps
		unsigned long long PTS=0,DTS=0,ESCR=0;
		unsigned char *pPTS=NULL,*pDTS=NULL,*pESCR=NULL;

		// PTS, if present
		if (PTS_DTS_flags == 2 || PTS_DTS_flags == 3) {	// '10' or '11'
			if ((*hdr >> 4) != PTS_DTS_flags) {	// should be followed by '0010' if '10' or '0011' if '11'
				fprintf(stderr,"PTS_DTS_flags = %u, next is %u not %u\n",
					PTS_DTS_flags,*hdr >> 4,PTS_DTS_flags);
				return 0;
			}

			if (	(hdr[0] & 0x01) == 0 ||
				(hdr[2] & 0x01) == 0 ||
				(hdr[4] & 0x01) == 0) {
				fprintf(stderr,"Marker bits in PTS timestamp don't line up\n");
				return 0;
			}

			pPTS =	hdr;
			PTS =	(((unsigned long long)((hdr[0] >> 1) & 0x07)) << (30+9)) |
				(((unsigned long long)( hdr[1]             )) << (22+9)) |
				(((unsigned long long)((hdr[2] >> 1) & 0x7F)) << (15+9)) |
				(((unsigned long long)( hdr[3]             )) <<  (7+9)) |
				(((unsigned long long)((hdr[4] >> 1) & 0x7F))          );

			hdr += 5;
		}
		// DTS if present
		if (PTS_DTS_flags == 3) {
			if ((*hdr >> 4) != 1) {
				fprintf(stderr,"PTS_DTS_flags = %u, next is %u not 1\n",
					PTS_DTS_flags,*hdr >> 4);
				return 0;
			}

			if (	(hdr[0] & 0x01) == 0 ||
				(hdr[2] & 0x01) == 0 ||
				(hdr[4] & 0x01) == 0) {
				fprintf(stderr,"Marker bits in DTS timestamp don't line up\n");
				return 0;
			}

			pDTS =	hdr;
			DTS =	(((unsigned long long)((hdr[0] >> 1) & 0x07)) << (30+9)) |
				(((unsigned long long)( hdr[1]             )) << (22+9)) |
				(((unsigned long long)((hdr[2] >> 1) & 0x7F)) << (15+9)) |
				(((unsigned long long)( hdr[3]             )) <<  (7+9)) |
				(((unsigned long long)((hdr[4] >> 1) & 0x7F))          );

			hdr += 5;
		}
		// ESCR
		if (ESCR_flag) {
			if (	(hdr[0] & 0x04) == 0 ||
				(hdr[2] & 0x04) == 0 ||
				(hdr[4] & 0x04) == 0 ||
				(hdr[5] & 0x01) == 0) {
				fprintf(stderr,"Marker bits in ESCR don't line up\n");
				return 0;
			}

			pESCR =	hdr;
			ESCR =	(((unsigned long long)((hdr[0] >> 3) & 0x03)) << (30+9)) |
				(((unsigned long long)( hdr[0]       & 0x03)) << (28+9)) |
				(((unsigned long long)( hdr[1]             )) << (20+9)) |
				(((unsigned long long)((hdr[2] >> 3) & 0x1F)) << (15+9)) |
				(((unsigned long long)( hdr[2]       & 0x03)) << (13+9)) |
				(((unsigned long long)( hdr[3]             )) <<  (5+9)) |
				(((unsigned long long)((hdr[4] >> 3) & 0x1F)) <<    (9)) |
				(((unsigned long long)( hdr[4]       & 0x03)) <<    (7)) |
				(((unsigned long long)((hdr[5] >> 1) & 0x7F))          );
		}

		// perform the adjustment
		PTS  += last_SCR_difference;
		DTS  += last_SCR_difference;
		ESCR += last_SCR_difference;

		// patch the new values back in
		if (pPTS != NULL) {
			pPTS[0] = (pPTS[0] & ~(0x07 << 1)) | (((PTS >> (30+9)) & 0x07) << 1);
			pPTS[1] =                               PTS >> (22+9);
			pPTS[2] = (pPTS[2] & ~(0x7F << 1)) | (((PTS >> (15+9)) & 0x7F) << 1);
			pPTS[3] =                               PTS >>  (7+9);
			pPTS[4] = (pPTS[4] & ~(0x7F << 1)) | (((PTS >>     9 ) & 0x7F) << 1);
		}
		if (pDTS != NULL) {
			pDTS[0] = (pDTS[0] & ~(0x07 << 1)) | (((DTS >> (30+9)) & 0x07) << 1);
			pDTS[1] =                               DTS >> (22+9);
			pDTS[2] = (pDTS[2] & ~(0x7F << 1)) | (((DTS >> (15+9)) & 0x7F) << 1);
			pDTS[3] =                               DTS >>  (7+9);
			pDTS[4] = (pDTS[4] & ~(0x7F << 1)) | (((DTS >>     9 ) & 0x7F) << 1);
		}
		if (pESCR != NULL) {
			pESCR[0] = (pESCR[0] & ~(0x03 << 3)) | (((ESCR >> (30+9)) & 0x03) << 3);
			pESCR[0] = (pESCR[0] & ~(0x03     )) | (((ESCR >> (28+9)) & 0x03)     );
			pESCR[1] =                                ESCR >> (20+9);
			pESCR[2] = (pESCR[2] & ~(0x1F << 3)) | (((ESCR >> (15+9)) & 0x1F) << 3);
			pESCR[2] = (pESCR[2] & ~(0x03     )) | (((ESCR >> (13+9)) & 0x03)     );
			pESCR[3] =                                ESCR >>  (5+9);
			pESCR[4] = (pESCR[4] & ~(0x1F << 3)) | (((ESCR >>    (9)) & 0x1F) << 3);
			pESCR[4] = (pESCR[4] & ~(0x03     )) | (((ESCR >>    (7)) & 0x03)     );
			pESCR[5] = (pESCR[5] & ~(0x7F << 1)) | (((ESCR          ) & 0x7F) << 1);
		}

		// the Pinnacle MovieBox is so god damn finicky we must strip things out
		// or it will stall and freeze between MPEGs if we're not careful.
		if (syncword == 0x000001E0) {
			StripThings(payload,fence);
		}
	}
	else {			// MPEG-1
		while (hdr < fence && *hdr == 0xFF) hdr++;	// MPEG-1 stuffing
		if (hdr >= fence) return 0;
		// TODO: Handle MPEG-1
		return 0;
	}

	// make sure there's room
	if ((mpeg_outi+pkt_len+6) > sizeof(mpeg_out))
		FlushMPEGOut();
	if ((mpeg_outi+pkt_len+6) > sizeof(mpeg_out)) {
		fprintf(stderr,"MPEG output packet overflow, dropping packet\n");
		return 0;
	}

	// stuff it in
	mpeg_out[mpeg_outi++] = syncword >> 24;
	mpeg_out[mpeg_outi++] = syncword >> 16;
	mpeg_out[mpeg_outi++] = syncword >>  8;
	mpeg_out[mpeg_outi++] = syncword      ;
	memcpy(mpeg_out+mpeg_outi,buf,pkt_len+2);
	if (skipped != NULL) *skipped = pkt_len+2;
	mpeg_outi += pkt_len+2;

	if (mpeg_outi >= 2048)
		FlushMPEGOut();

	return 1;
}

// magic reset string that can be sent in
static char *reset_string = "[RESET MPEG NOW]";
int reset_string_i=0;
int reset_ding=0;

static int first_BB=0;
void MPEGInput(unsigned char *cbuf,int clen)
{
	unsigned char *buf;
	int len,skip;

	if ((mpeg_in_remain+clen) <= 4096) {
		buf = mpeg_in;
		len = mpeg_in_remain;
	}
	else {
		fprintf(stderr,"MPEG processing error: Buffer input overflow. Data dropped\n");
		buf = mpeg_in;
		len = 0;
	}

	memcpy(buf+len,cbuf,clen);
	len += clen;
	while (len > 0) {
		// scan for magic reset sequence
		if (*buf == reset_string[reset_string_i]) {
			reset_string_i++;
			if (reset_string[reset_string_i] == 0) {
				fprintf(stderr,"Received reset string\n");
				reset_string_i=0;
				reset_ding++;
			}
		}

		if (mpeg_state == 0) {		// looking for sync pattern
			mpeg_sync = (mpeg_sync << 8) | *buf++; len--;
			if (	mpeg_sync == 0x000001BA ||	// pack header? (2.5.3.3)
				mpeg_sync == 0x000001BB ||	// system header? (2.5.3.5)
				mpeg_sync == 0x000001C0 ||	// audio stream?
				mpeg_sync == 0x000001E0)	// video stream?
				mpeg_state = mpeg_sync;		// LOL how clever of me :)
			else if (mpeg_sync == 0x000001BD) {
				// no we do not use Dobly Digital AC-3 or other formats
				if (!warn_nonmpa) {
					fprintf(stderr,"WARNING: MPEG stream has private stream BD. This daemon does not support AC-3 audio.\n");
					warn_nonmpa = 1;
				}
			}
		}
		else if (mpeg_state == 0x000001BB) {
			if (!first_BB) {
				mpeg_out[mpeg_outi++] = 0x00;
				mpeg_out[mpeg_outi++] = 0x00;
				mpeg_out[mpeg_outi++] = 0x01;
				mpeg_out[mpeg_outi++] = 0xBB;
				memcpy(mpeg_out+mpeg_outi,buf,8);
				mpeg_outi += 8;
				FlushMPEGOut();
				first_BB=1;
			}

			// throw these away
			mpeg_state = 0;
		}
		else if (mpeg_state == 0x000001E0) {
			// we assume 2048 byte/packet PES packets.
			// to keep our sanity require that at least 2048 bytes are known.
			if (len < 2048) break;
			if (CheckModPacket(buf,len,mpeg_state,&skip)) {
				buf += skip;
				len -= skip;
			}
			mpeg_state = 0;
		}
		else if (mpeg_state == 0x000001C0) {
			// ditto (see comments for 0x000001E0)
			if (len < 2048) break;
			if (CheckModPacket(buf,len,mpeg_state,&skip)) {
				buf += skip;
				len -= skip;
			}
			mpeg_state = 0;
		}
		else if (mpeg_state == 0x000001BA) {
			unsigned long long SCR = 0;
			unsigned char header[10];
			int MPEG2 = 0,hlen = 0;

			// processing of pack header.
			// don't bother if there's not enough to parse.
			if (len < 24) break;

			// okay, so is this an MPEG-1 pack or MPEG-2 pack?
			memcpy(header,buf,10);
			if ((*buf >> 6) == 1) {	// MPEG-2 '01'
				MPEG2 = 1;
				hlen = 10;

				// header[0] = {
				//   '01'			 2 bits
				//   SCR[32...30]		 3 bits
				//   marker			 1 bit
				//   SCR[29...28]		 2 bits
				// };
				// header[1] = {
				//   SCR[27...20]		 8 bits
				// };
				// header[2] = {
				//   SCR[19...15]		 5 bits
				//   marker			 1 bit
				//   SCR[14...13]		 2 bits
				// };
				// header[3] = {
				//   SCR[12...5]		 8 bits
				// };
				// header[4] = {
				//   SCR[4...0]			 5 bits
				//   marker			 1 bit
				//   SCRext[8...7]		 2 bits
				// };
				// header[5] = {
				//   SCRext[6...0]		 7 bits
				//   marker			 1 bit
				// };
				// header[6] = {
				//   muxrate[21...14]		 8 bits
				// };
				// header[7] = {
				//   muxrate[13...6]		 8 bits
				// };
				// header[8] = {
				//   muxrate[5...0]		 6 bits
				//   marker			 1 bit
				//   marker			 1 bit
				// };
				// header[9] = {
				//   reserved			 5 bits
				//   pack_stuffing_length	 3 bits
				// };
				// ignoring the padding, we get 10 bytes
				if (	(header[0] & 0x04) == 0 ||
					(header[2] & 0x04) == 0 ||
					(header[4] & 0x04) == 0 ||
					(header[5] & 0x01) == 0 ||
					(header[8] & 0x03) != 3)
					continue;		// marker bits fail, junk

				SCR =	(((unsigned long long)((header[0] >>  3) & 0x07)) << 39) |
					(((unsigned long long)( header[0]        & 0x03)) << 37) |
					(((unsigned long long)( header[1]              )) << 29) |
					(((unsigned long long)((header[2] >>  3) & 0x1F)) << 24) |
					(((unsigned long long)( header[2]        & 0x03)) << 22) |
					(((unsigned long long)( header[3]              )) << 14) |
					(((unsigned long long)((header[4] >>  3) & 0x1F)) <<  9) |
					(((unsigned long long)( header[4]        & 0x03)) <<  7) |
					(((unsigned long long)((header[5] >>  1) & 0x7F))      );
			}
			else if ((*buf >> 4) == 2) { // MPEG-1 '0010'
				hlen = 8;
				MPEG2 = 0;
				// header[0] = {
				//   '0010'			 4 bits
				//   SCR[32...30]		 3 bits
				//   marker			 1 bit
				// };
				// header[1...2] = {
				//   SCR[29...15]		15 bits
				//   marker			 1 bit
				// };
				// header[3...4] = {
				//   SCR[14...0]		15 bits
				//   marker			 1 bit
				// };
				// header[5...7] = {
				//   marker			 1 bit
				//   mux_rate			22 bits
				//   marker			 1 bit
				// };
				if (	(header[0] &    1) == 0 ||
					(header[2] &    1) == 0 ||
					(header[4] &    1) == 0 ||
					(header[5] & 0x80) == 0 ||
					(header[7] &    1) == 0)
					continue;	// marker bits fail, it's junk

				// shift over by 9 to convert 90KHz to 27MHz
				SCR =	(((unsigned long long)((header[0] >>  1) & 0x07)) << (30+9)) |
					(((unsigned long long)( header[1]              )) << (22+9)) |
					(((unsigned long long)((header[2] >>  1) & 0x7F)) << (15+9)) |
					(((unsigned long long)( header[3]              )) <<  (7+9)) |
					(((unsigned long long)((header[4] >>  1) & 0x7F)) <<  (0+9));
			}
			else {
				// it's nonsense. chuck it and move on.
				mpeg_state = 0;
				continue;
			}

			unsigned long long delta = 0;
			if (SCR < last_SCR || SCR > (last_SCR+270000000LL))
				delta = last_SCR_delta;
			else
				delta = last_SCR_delta = (SCR - last_SCR);

			monotonic_SCR += delta;
			last_SCR_difference = monotonic_SCR - SCR;	// this is needed also for proper
									// PTS/DTS timestamp adjustment
			last_SCR = SCR;
			SCR = monotonic_SCR;

			// patch in the new SCR value
			if (MPEG2) {
				header[0] = (header[0] & ~(0x07 <<  3)) | (((SCR >> 39) & 0x07) <<  3);
				header[0] = (header[0] &  ~0x03       ) | (((SCR >> 37) & 0x03)      );
				header[1] =                                  SCR >> 29;
				header[2] = (header[2] & ~(0x1F <<  3)) | (((SCR >> 24) & 0x1F) <<  3);
				header[2] = (header[2] &  ~0x03       ) | (((SCR >> 22) & 0x03)      );
				header[3] =                                  SCR >> 14;
				header[4] = (header[4] & ~(0x1F <<  3)) | (((SCR >>  9) & 0x1F) <<  3);
				header[4] = (header[4] &  ~0x03       ) | (((SCR >>  7) & 0x03)      );
				header[5] = (header[5] & ~(0x7F <<  1)) | (( SCR        & 0x7F)      );
			}
			else {
				header[0] = (header[0] & ~(0x07 <<  1)) | (((SCR >> 39) & 0x07) <<  1);
				header[1] =                                  SCR >> 31;
				header[2] = (header[2] & ~(0x7F <<  1)) | (((SCR >> 24) & 0x7F) <<  1);
				header[3] =                                  SCR >> 16;
				header[4] = (header[4] & ~(0x7F <<  1)) | (((SCR >>  9) & 0x7F) <<  1);
			}

			FlushMPEGOut();

			// place the modified header in the buffer for output.
			// since this code is optimized for 2048 byte/packet streams,
			// and these streams have pack headers, we consider this
			// a reset of the buffer.
			mpeg_out[0] = mpeg_out[1] = 0x00;
			mpeg_out[2] = 0x01;
			mpeg_out[3] = 0xBA;
			memcpy(mpeg_out+4,header,hlen);
			mpeg_outi = 4 + hlen;
			buf += hlen;
			len -= hlen;
			mpeg_state = 0;
		}
		else {
			fprintf(stderr,"MPEG processing error: Unknown state 0x%08X\n",mpeg_state);
			mpeg_state = 0;
		}
	}

	mpeg_in_remain = len;
	if (len > 0) memmove(mpeg_in,buf,len);
}

static void command(int argc,char **argv)
{
	if (argc < 1)
		return;

	if (!strcmp(argv[0],"volume")) {
		if (argc == 3) {
			int vl = -atoi(argv[1]);
			if (vl < 0)   vl = 0;
			if (vl > 255) vl = 255;
			int vr = -atoi(argv[2]);
			if (vr < 0)   vr = 0;
			if (vr > 255) vr = 255;
			PinnacleMovieBoxSetMasterVolume(vl,vr);
		}
	}
	else {
		fprintf(stderr,"Command pipe: Unknown command %s\n",argv[0]);
	}
}

static int cmd_tmpi=0;
static char cmd_tmp[64];
void CMDInput(unsigned char *buf,int len)
{
	char c,*cb = (char*)buf;	// so the C compiler shuts the hell up about signed/unsigned char typecasting

	while (len-- > 0) {
		c = *cb++;
		if (c >= 32 || c < 0) {
			if (cmd_tmpi < 63)
				cmd_tmp[cmd_tmpi++] = c;
		}
		else if (c == 10) {
			char *argv[32],*t;
			int argc=0;

			cmd_tmp[cmd_tmpi] = 0;
			argv[argc++] = cmd_tmp;
			while (argc < 31 && (t=strchr(argv[argc-1],' ')) != NULL) {
				while (*t == ' ') *t++ = 0;
				argv[argc++] = t;
			}
			argv[argc] = NULL;
			command(argc,argv);
			cmd_tmpi = 0;
		}
	}
}

int main(int argc,char **argv)
{
	int idle = 1;

	unsigned char mpeg[2048];
	int mpegi = 0;

	unsigned char input[2048];
	int rd;

	if (mkdir("/var/video",0777) < 0 && errno != EEXIST) {
		fprintf(stderr,"Cannot create /var/video\n");
		return 1;
	}
	if (mkfifo("/var/video/mpeg.pes.feed.fifo",0777) < 0 && errno != EEXIST) {
		fprintf(stderr,"Cannot create /var/video/mpeg pes fifo\n");
		return 1;
	}
	if (mkfifo("/var/video/command.fifo",0777) < 0 && errno != EEXIST) {
		fprintf(stderr,"Cannot create /var/video/command fifo\n");
		return 1;
	}

	int src_fd = open(pipename="/var/video/mpeg.pes.feed.fifo",O_RDONLY|O_NONBLOCK);
	if (src_fd < 0) return 1;
	int cmd_fd = open(cmdpipe="/var/video/command.fifo",O_RDONLY|O_NONBLOCK);
	if (cmd_fd < 0) return 1;
	int s;

	signal(SIGPIPE,sigma);
	signal(SIGQUIT,sigma);
	signal(SIGTERM,sigma);
	signal(SIGINT,sigma);

	// initialize libusb
	usb_init();
	usb_find_busses();
	usb_find_devices();

	if (PinnacleMovieBoxInit() < 0) {
		fprintf(stderr,"Cannot initialize Pinnacle MovieBox device\n");
		return 1;
	}

	// we need high priority in the system to ensure glitch-free playback
	nice(-20);
	while (!die) {
		idle = 1;
		if (PinnacleMovieBoxDeviceRemoved()) {
			fprintf(stderr,"Device was removed! Exiting now!\n");
			break;
		}

		// read MPEG input. For the MPEG handler's sanity, don't feed it
		// the stream until we have 2048 bytes ready.
		if (mpegi < 2048) {
			int rd = 2048 - mpegi;
			rd = read(src_fd,mpeg+mpegi,rd);
			if (rd > 0) {
				idle = 0;
				mpegi += rd;
			}
		}
		if (mpegi == 2048) {
			MPEGInput(mpeg,mpegi);
			mpegi = 0;
		}

		// read command input
		rd = read(cmd_fd,input,2048);
		if (rd > 0) {
			idle = 0;
			CMDInput(input,rd);
		}

		if (idle)
			usleep(1000);		// try not to suck up all CPU power

		if (reset_ding) {
			reset_ding=0;
			mpeg_outi=0;		// just throw the junk away on behalf of the stupid thing
		}
	}

	PinnacleMovieBoxFree();
	close(cmd_fd);
	close(src_fd);
	unlink("/var/video/mpeg.pes.feed.fifo");
	unlink("/var/video/command.fifo");
	rmdir("/var/video");
}

