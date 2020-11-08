#include "data_queue.h"
#include <SDL.h>
#include <pthread.h>
#define FRAME_QUEUE_SIZE 16

SDL_AudioDeviceID audio_dev;
void *sdl_audio_callback(void *param)
{
	frameQueue *fq = (frameQueue *)param;
	AVFrame *frame = av_frame_alloc();
	do{
		if(0 > frame_queue_get(fq, frame, 1)){
			destory_packet_queue(fq->pkt_queue);
			frame_queue_set_pkt_queue(fq, NULL);
			continue;
		}
		
	}while(1);
}

frameQueue *player_init()
{
	int flags = 0;
	flags = SDL_INIT_AUDIO;
	if(0 != SDL_Init(flags)){
		log_print("SDL_Init failed\n");
		return NULL;
	}
	packetQueue *pq = create_packet_queue();
	frameQueue *fq = create_frame_queue(FRAME_QUEUE_SIZE);
	SDL_AudioSpec wanted_spec, spec;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.silence = 0;
	wanted_spec.samples = 2048;
	wanted_spec.channels = 2;
	wanted_spec.freq = 44100;
	wanted_spec.callback = sdl_audio_callback;
	wanted_spec.userdata = (void *)fq;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHENNELS_CHANGE);
#if 0
	pthread_t pt;
	pthread_create(&pt, NULL, pcm_write_thread, (void *)fq);
#endif
	return fq;
}

void *decode_thread(void *param)
{

}

int player_start()
{
	SDL_CreateThread(decode_thread, "decoder", )
}

int main(int argc, void **argv)
{
	player_init();

	return 0;
}
