#include "data_queue.h"
#include <pthread.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "log.h"
#define FRAME_QUEUE_SIZE 16

const char *filename = "./waste_time.mp3";
int xrun_recovery(snd_pcm_t *handle, int err);
snd_pcm_t *pcm_init();

typedef struct player_ctrl{
	int stop:1;
	int pause:1;
	int eof:1;
	AVFormatContext *ic;
	AVCodecContext *avctx;
	packetQueue *pkt_queue;
	frameQueue *frame_queue;
}player_ctrl;



void *pcm_write_thread(void *param)
{
	frameQueue *fq = (frameQueue *)param;
    signed short *ptr;
    int err, cptr;

    snd_pcm_t *handle = pcm_init();
	AVFrame *frame = av_frame_alloc();
	while(1){
		frame_queue_get(fq, frame, 1);
		int data_len = frame->channels * frame->nb_samples * 2;
		log_print("frame info:len: %d channels: %d, channel_layout: %ld, quality: %d, pts: %ld, nb_samples: %d, sample_rate: %d\n", data_len, frame->channels, frame->channel_layout, frame->quality, frame->pts, frame->nb_samples, frame->sample_rate);
		log_print("linesize: %d\n", frame->linesize[0]);

		if(frame->format != 1 || frame->sample_rate != 1 || frame->channels != 1 || frame->channel_layout != 1){
		}

		SwrContext *swr_ctx = swr_alloc();
		swr_alloc_set_opts(swr_ctx, out_ch_layout, out_sample_fmt, out_sample_rate, in_ch_layout, in_sample_fmt, in_sample_rate, 0, NULL);
		sw_init(swr_ctx);
		sw_convert(swr_ctx, out_buf, out_samples, in_buf, in_samples);
		swr_free(&swr_ctx);
        while (cptr > 0) {
            err = snd_pcm_writei(handle, frame->extended_data, frame->linesize[0]);
            if (err == -EAGAIN)
                continue;
            if (err < 0) {
                if (xrun_recovery(handle, err) < 0) {
                    printf("Write error: %s\n", snd_strerror(err));
                    exit(EXIT_FAILURE);
                }
                break;  /* skip one period */
            }
        }
	}
}

player_ctrl *player_init()
{
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
	player->pkt_queue = pq;
	player->frame_queue = fq;
#if 1
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
			}else{
				printf("[%s %d] %d %d, EAGAIN:%d \n",__FILE__,__LINE__, AVERROR_EOF,AVERROR(EINVAL), AVERROR(EAGAIN));
			}
			if(ret == AVERROR(EINVAL)){
				log_print("avctx is not opened\n");
				break;
			}
			if(ret == AVERROR_EOF){
				log_print("codec end of file\n");
				break;
			}
		}while(ret != AVERROR(EAGAIN));

		if(packet_pending == 1 || packet_queue_get(player->pkt_queue, &pkt, !player->eof) > 0){
			log_print("packet position: %ld\n", pkt.pos);
			if(AVERROR(EAGAIN) == avcodec_send_packet(player->avctx, &pkt)){
				printf("[%s %d]\n",__FILE__,__LINE__);
				packet_pending = 1;
			}else{
				printf("avcodec send...\n");
				packet_pending = 0;
				av_packet_unref(&pkt);
			}
		}else{
			printf("[%s %d] %d, ----%u\n",__FILE__,__LINE__, packet_pending, (unsigned)(player->eof));
			if(packet_pending == 0 && (player->eof & 0x1)){
				log_print("packet end of file\n");
				break;
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

	avctx = avcodec_alloc_context3(NULL);
	if(avctx == NULL){
		log_print("avcocde_alloc_context3 failed\n");
		return AVERROR(ENOMEM);
	}
	ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
	printf("[%s %d]samples: %d channels: %d channel_layout: %ld\n", __FILE__, __LINE__, avctx->sample_rate, avctx->channels, avctx->channel_layout);
	if(ret < 0){
		log_print("avcodec_parameters_to_context failed\n");
		goto fail;
	}
	codec = avcodec_find_decoder(avctx->codec_id);
	if(avcodec_open2(avctx, codec, NULL) < 0){
		log_print("could not open codec for input stream %d", stream_index);
		return -1;
	}
	player->avctx = avctx;

	avctx->pkt_timebase = ic->streams[stream_index]->time_base;
	avctx->codec_id = codec->id;
	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

	int sample_rate = avctx->sample_rate;
	int channels = avctx->channels;
	uint64_t channel_layout = avctx->channel_layout;
	log_print("sample rate: %d, channels: %d, channel_layout: %ld\n", sample_rate, channels, channel_layout);
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
				log_print("input end....................\n");
				break;
			}
		}else{
			log_print("packet put %d ...\n", player->pkt_queue->nb_packets);
			log_print("packet info: pts: %ld, dts: %ld, size: %d, duration: %ld, pos: %ld\n", pkt.pts, pkt.dts, pkt.size, pkt.duration, pkt.pos);
			AVBufferRef *buf = pkt.buf;
			if(buf){
				log_print("buffer size: %d, ref number: %d\n", buf->size, *(int *)((int *)buf->buffer + sizeof(char *) + sizeof(int)));
			}
			packet_queue_put(player->pkt_queue, &pkt);
			//av_packet_unref(&pkt);
		}
	}

	avformat_close_input(&ic);
	log_print("read thread end.....\n");
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

	//if no this call, mp3 decoder report "Header missing"
	err = avformat_find_stream_info(ic, NULL);
	if(err < 0){
		log_print("could not find codec parameters\n");
		return -1;
	}

#if 0
	int i;
	for(i = 0; i < ic->nb_streams; i++){
		AVStream *stream = ic->streams[i];
		AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
		if(codec == NULL){
			log_print("decoder find failed\n");
			continue;
		}
		AVCodecContext *avctx;
		avctx = avcodec_alloc_context3(codec);
		if(avctx == NULL){
			log_print("avcocde_alloc_context3 failed\n");
			return AVERROR(ENOMEM);
		}
		int ret = avcodec_parameters_to_context(avctx, stream->codecpar);
		if(ret < 0){
			log_print("avcodec_parameters_to_context failed\n");
			return -1;
		}
		if(avcodec_open2(avctx, codec, NULL) < 0){
			log_print("could not open codec for input stream %d", i);
			return -1;
		}
		player->avctx = avctx;
	}
#endif
	//stream_index = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	player->ic = ic;
	stream_component_open(player, 0);
	pthread_t pt, pt2;

	pthread_create(&pt, NULL, read_thread, (void *)player);
	pthread_create(&pt2, NULL, decode_thread, (void *)player);
	//SDL_CreateThread(decode_thread, "decoder", (void *)player);
}

int main(int argc, void **argv)
{
	if(argc > 1){
		filename = argv[1];
		log_print("filename: %s\n", filename);
	}
	player_ctrl *player = player_init();
	player_start(player);

	while(1)
		sleep(100000);

	return 0;
}
