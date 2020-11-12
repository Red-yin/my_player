#include "data_queue.h"
#include <SDL2/SDL.h>
#include <pthread.h>
#include <unistd.h>
#include "libavformat/avformat.h"
#include "log.h"
#define FRAME_QUEUE_SIZE 16

const char *filename = "./alarm.mp3";

typedef struct player_ctrl{
	int stop:1;
	int pause:1;
	int eof:1;
	AVFormatContext *ic;
	AVCodecContext *avctx;
	packetQueue *pkt_queue;
	frameQueue *frame_queue;
}player_ctrl;

#if 1
SDL_AudioDeviceID audio_dev;
void sdl_audio_callback(void *param, Uint8 *stream, int len)
{
	frameQueue *fq = (frameQueue *)param;
	AVFrame *frame = av_frame_alloc();
	do{
		if(0 > frame_queue_get(fq, frame, 1)){
			destory_packet_queue(fq->pkt_queue);
			frame_queue_set_pkt_queue(fq, NULL);
			continue;
		}
		int data_len = av_samples_get_buffer_size(NULL, frame->channels, frame->nb_samples, frame->format, 1);
		printf("frame len: %d\n", data_len);
		int len1 = data_len < len ? data_len: len;
		SDL_MixAudioFormat(stream, (uint8_t *)frame->data[0], AUDIO_S16SYS, len1, SDL_MIX_MAXVOLUME);
	}while(1);
}
#endif

player_ctrl *player_init()
{
#if 1
	int flags = 0;
	flags = SDL_INIT_AUDIO;
	if(0 != SDL_Init(flags)){
		log_print("SDL_Init failed\n");
		return NULL;
	}
#endif
	//avdevice_register_all();
	avformat_network_init();
	av_register_all();

	player_ctrl *player = (player_ctrl *)calloc(1, sizeof(player_ctrl));
	if(player == NULL){
		log_print("player calloc failed\n");
		return NULL;
	}
	packetQueue *pq = create_packet_queue();
	frameQueue *fq = create_frame_queue(FRAME_QUEUE_SIZE, pq);
#if 1
	SDL_AudioSpec wanted_spec, spec;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.silence = 0;
	wanted_spec.samples = 2048;
	wanted_spec.channels = 2;
	wanted_spec.freq = 44100;
	wanted_spec.callback = sdl_audio_callback;
	wanted_spec.userdata = (void *)fq;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
#endif

	player->pkt_queue = pq;
	player->frame_queue = fq;
#if 0
	pthread_t pt;
	pthread_create(&pt, NULL, pcm_write_thread, (void *)fq);
#endif
	return player;
}

void *decode_thread(void *param)
{
	player_ctrl *player = (player_ctrl *)param;
	int got_frame, packet_pending = 0, ret;
	AVPacket pkt;
	AVFrame *frame = av_frame_alloc();
	if(frame == NULL){
		log_print("av_frame_alloc failed\n");
		goto end;
	}

	while(1){
		if(player->avctx == NULL){
			log_print("avctx is NULL\n");
			break;
		}
		do{
			ret = avcodec_receive_frame(player->avctx, frame);
			printf("[%s %d]ret: %d\n",__FILE__,__LINE__, ret);
			if(ret >= 0){
				frame_queue_put(player->frame_queue, frame, 0);
			}
			printf("[%s %d] %d %d, EAGAIN:%d \n",__FILE__,__LINE__, AVERROR_EOF,AVERROR(EINVAL), AVERROR(EAGAIN));
			if(ret == AVERROR(EINVAL)){
				log_print("avctx is not opened\n");
				break;
			}
			if(ret == AVERROR_EOF){
				//decode end of file
			}
		}while(ret != AVERROR(EAGAIN));

			printf("[%s %d]\n",__FILE__,__LINE__);
		if(packet_pending == 1 || packet_queue_get(player->pkt_queue, &pkt, 1) >= 0){
			if(AVERROR(EAGAIN) == avcodec_send_packet(player->avctx, &pkt)){
				packet_pending = 1;
			}else{
				printf("avcodec send...\n");
				packet_pending = 0;
				av_packet_unref(&pkt);
			}
		}
	}
end:
	av_frame_free(&frame);
	return NULL;
}

int stream_component_open(player_ctrl *player, int stream_index)
{
	AVFormatContext *ic = player->ic;
	AVCodecContext *avctx;
	AVCodec *codec;
	AVPacket pkt;
	int ret;
	if(stream_index < 0 || stream_index >= ic->nb_streams)
		return -1;

	AVCodec *vc = avcodec_find_decoder(ic->streams[stream_index]->codecpar->codec_id);
	avctx = avcodec_alloc_context3(vc);
	if(avctx == NULL){
		log_print("avcocde_alloc_context3 failed\n");
		return AVERROR(ENOMEM);
	}
	ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
	if(ret < 0){
		log_print("avcodec_parameters_to_context failed\n");
		goto fail;
	}
	avctx->pkt_timebase = ic->streams[stream_index]->time_base;
	codec = avcodec_find_decoder(avctx->codec_id);
	avctx->codec_id = codec->id;
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

	int sample_rate = avctx->sample_rate;
	int channels = avctx->channels;
	uint64_t channel_layout = avctx->channel_layout;
	player->avctx = avctx;

	return 0;
fail:
	log_print("[%s %d] open faild\n",__FILE__,__LINE__);
	avcodec_free_context(&avctx);
	return -1;
}

int decode_interrupt_cb(void *ctx)
{
	player_ctrl *player = (player_ctrl *)ctx;
	return player->stop;
}

void *read_thread(void *param)
{
	int ret;
	player_ctrl *player = (player_ctrl *)param;
	AVFormatContext *ic = player->ic;
	player->eof = 0;
	AVPacket pkt;
	while(1){
		if(player->stop){
			log_print("stop...\n");
			break;
		}
		ret = av_read_frame(ic, &pkt);
		if(ret < 0){
			if(ret == AVERROR_EOF || avio_feof(ic->pb)){
				//packet_queue_put_nullpacket();
				player->eof = 1;
				log_print("input end...\n");
				break;
			}
		}else{
			packet_queue_put(player->pkt_queue, &pkt);
			av_packet_unref(&pkt);
		}
	}

	avformat_close_input(&ic);
}

int player_start(player_ctrl *player)
{
	if(player == NULL){
		return -1;
	}
	int err, stream_index;
	AVFormatContext *ic = avformat_alloc_context();
	if(ic == NULL){
		log_print("avformat alloc failed\n");
		return -1;
	}
	ic->interrupt_callback.callback = decode_interrupt_cb;
	ic->interrupt_callback.opaque = (void *)player;
	err = avformat_open_input(&ic, filename, NULL, NULL);
	if(err < 0){
		log_print("%s open input failed: %d\n", filename, err);
		return -1;
	}
	stream_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	player->ic = ic;
	stream_component_open(player, 0);
	pthread_t pt, pt2;

	pthread_create(&pt, NULL, read_thread, (void *)player);
	//pthread_create(&pt2, NULL, decode_thread, (void *)player);
	SDL_CreateThread(decode_thread, "decoder", (void *)player);
}

int main(int argc, void **argv)
{
	player_ctrl *player = player_init();
	player_start(player);

	while(1)
		sleep(100000);

	return 0;
}
