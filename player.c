#include "data_queue.h"
#include <pthread.h>
#include <semaphore.h>
#include <libswresample/swresample.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "log.h"
#define FRAME_QUEUE_SIZE 16

const char *filename = "./waste_time.mp3";
int xrun_recovery(snd_pcm_t *handle, int err);
snd_pcm_t *pcm_init();

typedef enum player_status{
	PLAYER_IDLE,
	PLAYER_STOP,
	PLAYER_PAUSE,
	PLAYER_PLAYING
}player_status;

typedef struct play_list{
	char *url;
	struct play_list *next;
}play_list;

typedef struct play_task{
	int stop:1;
	int pause:1;
	int eof:1;
	sem_t sem;
}play_task;

typedef struct play_list_ctrl{
	play_list *first;
	play_list *last;
	play_list *current;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
}play_list_ctrl;

typedef enum play_list_loop{
	ORDER,
	ALL_REPEAT,
	SHUFFLE,
	REPEAT_ONCE,
	ONCE
}play_list_loop;

typedef struct player_ctrl{
	player_status status;
	play_task *task;
	play_list_ctrl *list;
	play_list_loop loop_type;

	AVFormatContext *ic;
	AVCodecContext *avctx;
	packetQueue *pkt_queue;
	frameQueue *frame_queue;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
}player_ctrl;



void *pcm_write_thread(void *param)
{
	frameQueue *fq = (frameQueue *)param;
    signed short *ptr;
    int err, cptr;

    snd_pcm_t *handle = pcm_init();
	AVFrame *frame = av_frame_alloc();
	uint8_t *out_buf;
	while(1){
		frame_queue_get(fq, frame, 1);
		int data_len = frame->channels * frame->nb_samples * 2;
		log_print("frame info:len: %d channels: %d, channel_layout: %ld, quality: %d, pts: %ld, nb_samples: %d, sample_rate: %d\n", data_len, frame->channels, frame->channel_layout, frame->quality, frame->pts, frame->nb_samples, frame->sample_rate);
		log_print("linesize: %d\n", frame->linesize[0]);

		if(frame->format != 1 || frame->sample_rate != 1 || frame->channels != 1 || frame->channel_layout != 1){
		}

		SwrContext *swr_ctx = swr_alloc();
		swr_alloc_set_opts(swr_ctx, av_get_default_channel_layout(1), SND_PCM_FORMAT_S16, 44100, frame->channel_layout, frame->format, frame->sample_rate, 0, NULL);
		swr_init(swr_ctx);
		int out_len = av_samples_get_buffer_size(NULL, 1, frame->nb_samples, SND_PCM_FORMAT_S16, 0);
		out_buf = (char *)malloc(out_len);
		log_print("swr out len: %d\n", out_len);
		swr_convert(swr_ctx, &out_buf, frame->nb_samples, frame->extended_data, frame->nb_samples);
		swr_free(&swr_ctx);
        while (out_len > 0) {
            err = snd_pcm_writei(handle, out_buf, out_len);
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

int task_signal(play_task *task)
{
	if(task == NULL){
		return -1;
	}
	sem_post(&task->sem);
	return 0;
}

void task_wait(play_task *task)
{
	if(task == NULL){
		return;
	}
	if (sem_wait(&task->sem) != 0){
		log_print("sem error\n");
		return;
	}
}

void *decode_thread(void *param)
{
	player_ctrl *player = (player_ctrl *)param;
	int got_frame, packet_pending = 0, ret;
	AVPacket pkt;
	play_task *task = NULL;
	AVFrame *frame = av_frame_alloc();
	if(frame == NULL){
		log_print("av_frame_alloc failed\n");
		goto end;
	}

	while(1){
		if(player->avctx == NULL){
			log_print("avctx is NULL\n");
			continue;
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

		if(packet_pending == 1 || packet_queue_get(player->pkt_queue, &pkt, 1) > 0){
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
			printf("[%s %d] %d, ----\n",__FILE__,__LINE__, packet_pending);
			if(packet_pending == 0){
				if(task->stop){
					clean_frame_queue(player->frame_queue);
					//TODO: task stop finished
					player->status = PLAYER_STOP;
					task_signal(task);
				}
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
	play_task *task = (play_task *)ctx;
	return task->stop;
}

void *read_thread(void *param)
{
	int ret;
	player_ctrl *player = (player_ctrl *)param;
	AVFormatContext *ic = player->ic;
	play_task *task = player->task;
	AVPacket pkt;
	while(1){
		if(task == NULL || task->stop || task->eof){
			log_print("player ctrl wait...\n");
			if(task && task->stop){
				clean_packet_queue(player->pkt_queue);
				packet_queue_signal(player->pkt_queue);
			}
			//TODO: wait
			pthread_mutex_lock(&player->mutex);
			pthread_cond_wait(&player->cond, &player->mutex);
			//after wait
			ic = player->ic;
			task = player->task;
			pthread_mutex_unlock(&player->mutex);
		}
		ret = av_read_frame(ic, &pkt);
		if(ret < 0){
			if(ret == AVERROR_EOF || avio_feof(ic->pb)){
				//packet_queue_put_nullpacket();
				task->eof = 1;
				log_print("input end....................\n");
				continue;
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
#if 0
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
#endif

	pthread_t pt, pt2;
	pthread_create(&pt, NULL, read_thread, (void *)player);
	pthread_create(&pt2, NULL, decode_thread, (void *)player);
	//SDL_CreateThread(decode_thread, "decoder", (void *)player);
	new_task(player, filename);
}

int player_add_task(player_ctrl *player, const char *url)
{
	if(player == NULL || player->list == NULL || url == NULL || strlen(url) <= 0){
		return -1;
	}
	play_list *node = (play_list *)calloc(1, sizeof(play_list));
	if(node == NULL){
		log_print("play list calloc failed\n");
		return -1;
	}
	node->url = (char *)malloc(strlen(url) + 1);
	if(node->url == NULL){
		log_print("url malloc failed \n");
		free(node);
		return -1;
	}
	play_list_ctrl *list = player->list;
	pthread_mutex_lock(&list->mutex);
	if(list->first == NULL){
		list->first = node;
	}
	list->last = node;
	pthread_mutex_unlock(&list->mutex);
	pthread_cond_signal(&list->cond);
	return 0;
}

void play_list_destory(play_list *node)
{
	if(node){
		if(node->url){
			free(node->url);
		}
		free(node);
	}
}

int player_task_pause(play_task *task)
{
	if(task == NULL){
		return 0;
	}
	task->pause = 1;
	return 0;
}

int player_task_resume(play_task *task)
{
	if(task == NULL){
		return 0;
	}
	task->pause = 0;
	//sem post
	return 0;
}

int player_task_stop(play_task *task)
{
	if(task == NULL){
		return 0;
	}
	task->stop = 1;
	return 0;
}

int player_task_clean(player_ctrl *player)
{
	if(player == NULL || player->list == NULL){
		return -1;
	}
	if(player->list->first == NULL){
		return 0;
	}
	play_list_ctrl *list = player->list;
	pthread_mutex_lock(&list->mutex);
	play_list *p = list->first;
	play_list *next = NULL;
	while(p){
		next = p->next;
		play_list_destory(p);
		p = next;
	}
	list->current = NULL;
	pthread_mutex_lock(&list->mutex);
	return 0;
}

int player_command_stop(player_ctrl *player)
{
	player_task_stop(player->task);
	player_task_clean(player);
	player->task = NULL;
	return 0;
}

int player_command_pause(player_ctrl *player)
{
	player_task_pause(player->task);
	return 0;
}

int player_command_resume(player_ctrl *player)
{
	player_task_resume(player->task);
	return 0;
}

int player_command_play(player_ctrl *player, const char *url)
{
	if(player->status != PLAYER_IDLE){
		player_command_stop(player);
	}
	player_add_task(player, url);
	return 0;
}

int player_command_append(player_ctrl *player, const char *url)
{
	player_add_task(player, url);
	return 0;
}

int destory_task(play_task *task)
{
	if(task == NULL){
		return -1;
	}
	sem_destroy(&task->sem);
	free(task);
	return 0;
}

int new_task(player_ctrl *player, const char *url)
{
	if(player == NULL || url == NULL){
		return -1;
	}
	play_task *task = (play_task *)calloc(1, sizeof(play_task));
	if(task == NULL){
		log_print("task calloc failed\n");
		return -1;
	}
	sem_init(&task->sem, 0, 0);

	int err, stream_index;
	AVFormatContext *ic = avformat_alloc_context();
	if(ic == NULL){
		log_print("avformat alloc failed\n");
		return -1;
	}
	ic->interrupt_callback.callback = decode_interrupt_cb;
	ic->interrupt_callback.opaque = (void *)task;
	err = avformat_open_input(&ic, url, NULL, NULL);
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

	player->ic = ic;
	player->task = task;
	stream_component_open(player, 0);
	pthread_cond_signal(&player->cond);
}

void *player_task_handle(void *param)
{
	player_ctrl *player = (player_ctrl *)param;
	play_list_ctrl *list = player->list;
	char *url = NULL;
	while(1){
		pthread_mutex_lock(&list->mutex);
		if(list->first == NULL && list->last == NULL){
			pthread_cond_wait(&list->cond, &list->mutex);
		}
		if(list->current == NULL){
			list->current = list->first;
		}else{
			list->current = list->current->next;
		}
		if(list->current == NULL){
			pthread_mutex_unlock(&list->mutex);
			continue;
		}
		url = (char *)calloc(1, strlen(list->current->url) + 1);
		if(url == NULL){
			log_print("url calloc failed\n");
			exit(1);
		}
		memcpy(url, list->current->url, strlen(list->current->url));
		pthread_mutex_unlock(&list->mutex);

		new_task(player, url);
		free(url);
		task_wait(player->task);
		destory_task(player->task);
		player->task = NULL;
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
	player->list = (play_list_ctrl *)calloc(1, sizeof(play_list_ctrl));
	if(player->list == NULL){
		log_print("player list calloc failed\n");
		goto fail;
	}
	player->loop_type = 0;
	player->status = PLAYER_IDLE;
	packetQueue *pq = create_packet_queue();
	frameQueue *fq = create_frame_queue(FRAME_QUEUE_SIZE, pq);
	player->pkt_queue = pq;
	player->frame_queue = fq;
#if 1
	pthread_t pt, pt1;
	pthread_create(&pt, NULL, pcm_write_thread, (void *)fq);
	pthread_create(&pt1, NULL, player_task_handle, (void *)player);
#endif
	return player;
fail:
	if(player){
		free(player);
	}
	return NULL;
}

int main(int argc, void **argv)
{
	if(argc > 1){
		filename = argv[1];
		log_print("filename: %s\n", filename);
	}
	player_ctrl *player = player_init();
	player_start(player);

	while(1){
		printf("cli>");
		sleep(10000);
	}

	return 0;
}
