/* MP3 stubs so decoder.c links on the host without porting helix's
 * ARM-specific assembly.h. The MP3 decode path cannot run here; the
 * host tests cover FLAC (and WAV, which needs no helix) end-to-end.
 */
#include <stddef.h>
#include "mp3dec.h"

HMP3Decoder MP3InitDecoder(void)
{
	return NULL;
}

void MP3FreeDecoder(HMP3Decoder hMP3Decoder)
{
	(void)hMP3Decoder;
}

int MP3Decode(HMP3Decoder hMP3Decoder, unsigned char **inbuf, int *bytesLeft,
	short *outbuf, int useSize)
{
	(void)hMP3Decoder; (void)inbuf; (void)bytesLeft; (void)outbuf; (void)useSize;
	return -1;
}

void MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo)
{
	(void)hMP3Decoder;
	mp3FrameInfo->nChans = 0;
	mp3FrameInfo->outputSamps = 0;
	mp3FrameInfo->samprate = 0;
	mp3FrameInfo->bitrate = 0;
	mp3FrameInfo->layer = 0;
}

int MP3FindSyncWord(unsigned char *buf, int nBytes)
{
	(void)buf; (void)nBytes;
	return -1;
}
