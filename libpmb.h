
int PinnacleMovieBoxInit();
int PinnacleMovieBoxFree();
int PinnacleMovieBoxSetupPlayback();
int PinnacleMovieBoxWriteVideo(unsigned char *buf,int len);
int PinnacleMovieBoxWriteAudio(unsigned char *buf,int len);
int PinnacleMovieBoxSetMasterVolume(int l,int r);
int PinnacleMovieBoxDeviceRemoved();
int PinnacleMovieBoxReset();

int PinnacleMovieBoxEnableVideoOutputs(int flags);
#define PMB_VO_COMPOSITE		0x20
#define PMB_VO_SVIDEO_LUMA		0x10
#define PMB_VO_SVIDEO_CHROMA		0x08
#define PMB_VO_RGB_R			0x04
#define PMB_VO_RGB_G			0x02
#define PMB_VO_RGB_B			0x01

