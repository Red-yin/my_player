#include "data_queue.h"
#include <pthread.h>
#include <semaphore.h>
#include "libswresample/swresample.h"
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "pcm.h"
#include "slog.h"
#define FRAME_QUEUE_SIZE 16
#define PACKET_QUEUE_SIZE 64

enum AVSampleFormat ffmpeg_output_fmt = AV_SAMPLE_FMT_S16;
snd_pcm_format_t alsa_output_fmt = SND_PCM_FORMAT_S16;
int alsa_output_sample_rate = 44100;
int ffmpeg_output_sample_rate = 44100;
int alsa_output_channels = 2;
//int64_t ffmpeg_output_channel_layout = AV_CH_LAYOUT_MONO;
int64_t ffmpeg_output_channel_layout = AV_CH_LAYOUT_STEREO;
int ffmpeg_output_channels = 2;

typedef struct audioBuffer{
	char *buf;
	int len;
}audioBuffer;

typedef struct cycleNodeBuffer{
	audioBuffer *data;
	int max;
	int front;
	int rear;
}cycleNodeBuffer;


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

#if 0
typedef struct play_task{
	int stop:1;
	int pause:1;
	int eof:1;
	sem_t sem;
}play_task;
#endif

typedef struct play_list_ctrl{
	play_list *first;
	play_list *last;
	play_list *current;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
}play_list_ctrl;

typedef struct play_job{
	char *url;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
}play_job;

typedef enum play_list_loop{
	ORDER,
	ALL_REPEAT,
	SHUFFLE,
	REPEAT_ONCE,
	ONCE
}play_list_loop;

typedef struct player_ctrl{
	//player_status status;
	//play_task *task;
	//play_list_ctrl *list;
	//play_list_loop loop_type;
	play_job job;

	AVFormatContext *ic;
	AVCodecContext *avctx;
	packetQueue *pkt_queue;
	frameQueue *frame_queue;
	AudioParams audio_src;
	AudioParams audio_dst;

	pthread_mutex_t mutex;
	pthread_cond_t cond;
}player_ctrl;

void *pcm_write_thread(void *param)
{
	player_ctrl *player = (player_ctrl *)param;
	frameQueue *fq = (frameQueue *)player->frame_queue;
	signed short *ptr;
	int err, cptr;

	snd_pcm_t *handle = pcm_init(&player->audio_dst);
	AVFrame *frame = av_frame_alloc();
	SwrContext *swr_ctx = NULL;
	uint8_t *out_buf = av_malloc(1024*16);
#define SAVE_FILE_FLAG 0
#if SAVE_FILE_FLAG
	FILE *fp = fopen("thread.pcm", "w");
#endif
	while(1){
		while(player->pkt_queue->pause){
			usleep(100*1000);
		}
		frame_queue_get(fq, frame, 1);
		inf("frame get info:len: %d, pos: %ld, channels: %d, channel_layout: %ld, quality: %d, pts: %ld, nb_samples: %d, sample_rate: %d\n", frame->linesize[0], frame->pkt_pos, frame->channels, frame->channel_layout, frame->quality, frame->pts, frame->nb_samples, frame->sample_rate);

		inf("frame format: %d, channel_layout: %ld, sample_rate: %d, swr format: %d, channel_layout: %ld, sample_rate: %d\n", frame->format, frame->channel_layout, frame->sample_rate, player->audio_src.fmt, player->audio_src.channel_layout, player->audio_src.freq);
		//if(frame->format != player->audio_src.fmt || frame->sample_rate != player->audio_src.freq || frame->channels != player->audio_src.channels || frame->channel_layout != player->audio_src.channel_layout){
		if(swr_ctx){
			swr_free(&swr_ctx);
		}
		//swr_ctx = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, frame->sample_rate, frame->channel_layout, frame->format, frame->sample_rate, 0, NULL);
		swr_ctx = swr_alloc_set_opts(NULL, ffmpeg_output_channel_layout, ffmpeg_output_fmt, ffmpeg_output_sample_rate, frame->channel_layout, frame->format, frame->sample_rate, 0, NULL);
		if(swr_ctx == NULL || swr_init(swr_ctx) < 0){
			inf("Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
					frame->sample_rate, av_get_sample_fmt_name(frame->format), frame->channels,
					player->audio_dst.freq, av_get_sample_fmt_name(player->audio_dst.fmt), player->audio_dst.channels);
			swr_free(&swr_ctx);
			continue; 
		}
		player->audio_src.fmt = frame->format;
		player->audio_src.freq = frame->sample_rate;
		player->audio_src.channels = frame->channels;
		player->audio_src.channel_layout = frame->channel_layout;
		//}

		int out_len = av_samples_get_buffer_size(NULL, 2, frame->nb_samples, AV_SAMPLE_FMT_S16, 0);
		//int out_len = av_samples_get_buffer_size(NULL, player->audio_dst.channels, frame->nb_samples, player->audio_dst.fmt, 0);
		memset(out_buf, 0, sizeof(out_buf));
		if(swr_ctx){
			int len = swr_convert(swr_ctx, &out_buf, frame->nb_samples, frame->data, frame->nb_samples);
			inf("swr out len: %d, swr len: %d\n", out_len, len);
#if SAVE_FILE_FLAG
			fwrite(out_buf, 1, out_len, fp);
#else
			if(len > 0){
				char *p = out_buf;
				while (len > 0) {
					err = snd_pcm_writei(handle, p, len);
					if (err == -EAGAIN)
						continue;
					if (err < 0) {
						if (xrun_recovery(handle, err) < 0) {
							inf("Write error: %s\n", snd_strerror(err));
							exit(EXIT_FAILURE);
						}
						break;  /* skip one period */
					}
					len -= err;
					p += err;
					inf("pcm write left: %d\n", len);
				}
			}
#endif
		}
		av_frame_unref(frame);
	}
#if SAVE_FILE_FLAG
	fclose(fp);
#endif
}
#if 0
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
	inf("%s\n", __func__);
	if (sem_wait(&task->sem) != 0){
		inf("sem error\n");
		return;
	}
}
#endif

void *decode_thread(void *param)
{
	player_ctrl *player = (player_ctrl *)param;
	int got_frame, packet_pending = 0, ret;
	AVPacket pkt;
	AVFrame *frame = av_frame_alloc();
	if(frame == NULL){
		inf("av_frame_alloc failed\n");
		goto end;
	}
	while(1){
		if(packet_pending == 1 || packet_queue_get(player->pkt_queue, &pkt, 1) == 0){
			inf("packet get position: %ld\n", pkt.pos);
#if 0
			ret = avcodec_decode_audio4(player->avctx, frame, &got_frame, &pkt);
			if(ret >= 0){
				inf("[%s %d]frame put ret: %d, format: %d\n",__FILE__,__LINE__, ret, frame->format);
				frame_queue_put(player->frame_queue, frame, 1);
			}
#else
			ret = 0;
			while(player->avctx && ret >= 0){
				ret = avcodec_receive_frame(player->avctx, frame);
				if(ret >= 0){
					if(player->pkt_queue->abort_request){
						av_frame_unref(frame);
					}else{
						frame_queue_put(player->frame_queue, frame, 1);
					}
				}
#if 0
				else{
					inf("[%s %d] %d %d ret: %d, EAGAIN:%d \n",__FILE__,__LINE__, AVERROR_EOF,AVERROR(EINVAL), AVERROR(EAGAIN), ret);
				}
#endif
			}
			if(player->pkt_queue->abort_request){
				packet_pending = 0;
				av_packet_unref(&pkt);
				continue;
			}
			inf("avcodec ctx: %p", player->avctx);
			ret = avcodec_send_packet(player->avctx, &pkt);
			if(ret == 0){
				packet_pending = 0;
				av_packet_unref(&pkt);
			}else{
				if(ret == AVERROR(EAGAIN)){
					packet_pending = 1;
				}else{
					err("avcodec send packet failed: %d[AVERROR_EOF: %d, AVERROR(EINVAL): %d, AVERROR(ENOMEM): %d]", ret, AVERROR_EOF, AVERROR(EINVAL), AVERROR(ENOMEM));
					sleep(1);
				}
			}
#endif
		}
		else{
			if(packet_pending == 1){
				av_packet_unref(&pkt);
				packet_pending = 0;
			}
			if(player->pkt_queue->abort_request){
				clean_frame_queue(player->frame_queue);
				sem_post(&player->pkt_queue->stop_sem);
				inf("decode waiting");
				sem_wait(&player->pkt_queue->start_sem);
			}
		}
	}
end:
	if(frame)
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
		inf("avcocde_alloc_context3 failed\n");
		return AVERROR(ENOMEM);
	}
	ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
	inf("[%s %d]samples: %d channels: %d channel_layout: %ld, sample format: %d\n", __FILE__, __LINE__, avctx->sample_rate, avctx->channels, avctx->channel_layout, avctx->sample_fmt);
	if(ret < 0){
		inf("avcodec_parameters_to_context failed\n");
		goto fail;
	}
	codec = avcodec_find_decoder(avctx->codec_id);
	inf("ctx type: %d\n", avctx->codec_type);
	avctx->pkt_timebase = ic->streams[stream_index]->time_base;
	avctx->codec_id = codec->id;
	inf("codec id: %x\n", codec->id);
	if(avcodec_open2(avctx, codec, NULL) < 0){
		inf("could not open codec for input stream %d", stream_index);
		return -1;
	}
	player->avctx = avctx;

	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

	int sample_rate = avctx->sample_rate;
	int channels = avctx->channels;
	uint64_t channel_layout = avctx->channel_layout;
	inf("sample rate: %d, channels: %d, channel_layout: %ld\n", sample_rate, channels, channel_layout);
	player->avctx = avctx;

	return 0;
fail:
	inf("[%s %d] open faild\n",__FILE__,__LINE__);
	avcodec_free_context(&avctx);
	return -1;
}

int decode_interrupt_cb(void *ctx)
{
	packetQueue *pkt_queue = (packetQueue *)ctx;
	return pkt_queue->abort_request;
}

#define PROD_FLAG 1
int play_start(player_ctrl *player, const char *url);
void *read_thread(void *param)
{
	int ret, got_frame, err;
	player_ctrl *player = (player_ctrl *)param;
	AVFormatContext *ic = player->ic;
	AVPacket pkt;
	SwrContext *swr_ctx = NULL;
	uint8_t *out_buf = av_malloc(1024*16);
	AVFrame *frame = av_frame_alloc();
#if PROD_FLAG 
#else
	snd_pcm_t *handle = pcm_init(&player->audio_dst);
#endif
	FILE *fp = fopen("avcodec_receive.pcm", "w");

	while(1){
		inf("%s %d", __FILE__, __LINE__);
		if(player->pkt_queue->abort_request || player->pkt_queue->eof){
			if(ic)
				avformat_close_input(&ic);
			err("player->pkt_queue->abort_request: %d", player->pkt_queue->abort_request);
			if(player->pkt_queue->abort_request){
				clean_packet_queue(player->pkt_queue);
				packet_queue_signal(player->pkt_queue);
			}

			sem_wait(&player->pkt_queue->stop_sem);

			player->ic = NULL;
			pthread_mutex_lock(&player->job.mutex);
			while(player->ic == NULL){
				if(player->job.url != NULL){
					player->pkt_queue->abort_request = 0;
					play_start(player, player->job.url);
					free(player->job.url);
					player->job.url = NULL;
				}else{
					pthread_cond_wait(&player->job.cond, &player->job.mutex);
				}
			}
			pthread_mutex_unlock(&player->job.mutex);
			player->pkt_queue->eof = 0;
			sem_post(&player->pkt_queue->start_sem);

			ic = player->ic;
		}
		inf("%s %d", __FILE__, __LINE__);
		ret = av_read_frame(ic, &pkt);
		if(ret < 0){
		inf("%s %d", __FILE__, __LINE__);
			if(ret == AVERROR_EOF || avio_feof(ic->pb)){
				//packet_queue_put_nullpacket();
				player->pkt_queue->eof = 1;
				inf("input end....................\n");
				continue;
			}
		}else{
		inf("%s %d", __FILE__, __LINE__);
#if PROD_FLAG
			//inf("av read frame packet info: pts: %ld, dts: %ld, size: %d, duration: %ld, pos: %ld\n", pkt.pts, pkt.dts, pkt.size, pkt.duration, pkt.pos);
			//inf("file pos %ld ...\n", pkt.pos);
			packet_queue_put(player->pkt_queue, &pkt, 1);
			//av_packet_unref(&pkt);
#else
#if 0
			ret = avcodec_decode_audio4(player->avctx, frame, &got_frame, &pkt);
			inf("frame format: %d, channel_layout: %ld, sample_rate: %d, channels: %d, swr format: %d, channel_layout: %ld, sample_rate: %d", frame->format, frame->channel_layout, frame->sample_rate, frame->channels, player->audio_src.fmt, player->audio_src.channel_layout, player->audio_src.freq);
			inf("frame size: %d, frame format: %s", frame->linesize[0], av_get_sample_fmt_name(frame->format));
#if 0
			if(frame->format != player->audio_src.fmt || frame->sample_rate != player->audio_src.freq || frame->channels != player->audio_src.channels || frame->channel_layout != player->audio_src.channel_layout){
				if(swr_ctx){
					swr_free(&swr_ctx);
				}
				swr_ctx = swr_alloc_set_opts(NULL, av_get_default_channel_layout(player->audio_dst.channels), player->audio_dst.fmt, player->audio_dst.freq, frame->channel_layout, frame->format, frame->sample_rate, 0, NULL);
				if(swr_ctx == NULL || swr_init(swr_ctx) < 0){
					inf("Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
							frame->sample_rate, av_get_sample_fmt_name(frame->format), frame->channels,
							player->audio_dst.freq, av_get_sample_fmt_name(player->audio_dst.fmt), player->audio_dst.channels);
					swr_free(&swr_ctx);
					continue; 
				}
				player->audio_src.fmt = frame->format;
				player->audio_src.freq = frame->sample_rate;
				player->audio_src.channels = frame->channels;
				player->audio_src.channel_layout = frame->channel_layout;
			}
#endif
#else
			avcodec_send_packet(player->avctx, &pkt);
			ret = 0;
			while(player->avctx && ret >= 0){
				ret = avcodec_receive_frame(player->avctx, frame);
				if(ret >= 0){
					inf("frame format: %d, channel_layout: %ld, sample_rate: %d, channels: %d, swr format: %d, channel_layout: %ld, sample_rate: %d", frame->format, frame->channel_layout, frame->sample_rate, frame->channels, player->audio_src.fmt, player->audio_src.channel_layout, player->audio_src.freq);
					inf("frame size: %d, frame format: %s", frame->linesize[0], av_get_sample_fmt_name(frame->format));
					//fwrite(frame->data, 1, frame->linesize[0], fp);
#if 1
					memset(out_buf, 0, sizeof(out_buf));
					if(swr_ctx){
						swr_free(&swr_ctx);
					}
					int out_len = av_samples_get_buffer_size(NULL, 2, frame->nb_samples, AV_SAMPLE_FMT_S16, 0);
					swr_ctx = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, frame->sample_rate, frame->channel_layout, frame->format, frame->sample_rate, 0, NULL);
					//swr_ctx = swr_alloc_set_opts(NULL, av_get_default_channel_layout(player->audio_dst.channels), player->audio_dst.fmt, player->audio_dst.freq, frame->channel_layout, frame->format, frame->sample_rate, 0, NULL);
					swr_init(swr_ctx);
					if(swr_ctx){
						int len = swr_convert(swr_ctx, &out_buf, frame->nb_samples, frame->data, frame->nb_samples);
						inf("swr out len: %d, swr len: %d\n", out_len, len);
#if 0
						fwrite(out_buf, 1, out_len, fp);
#else
						if(len > 0){
							char *p = out_buf;
							while (len > 0) {
								err = snd_pcm_writei(handle, p, len);
								if (err == -EAGAIN)
									continue;
								if (err < 0) {
									if (xrun_recovery(handle, err) < 0) {
										inf("Write error: %s\n", snd_strerror(err));
										exit(EXIT_FAILURE);
									}
									break;  /* skip one period */
								}
								len -= err;
								p += err;
								inf("pcm write left: %d\n", len);
							}
						}
#endif
					}
#endif
				}
			}
#endif

#endif
#if 0
			int i;
			inf("================================\n");
			for(i = 0; i < frame->linesize[0]; i++){
				inf("%02x ", frame->extended_data[i]);
			}
			inf("\n");
#endif
		}
	}

	avformat_close_input(&ic);
	inf("read thread end.....\n");
	fclose(fp);
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

int player_clean_task(play_list_ctrl *list)
{
	if(list == NULL || list->first == NULL){
		return -1;
	}
	pthread_mutex_lock(&list->mutex);
	play_list *node = list->first;
	play_list *next = NULL;
	while(node){
		next = node->next;
		play_list_destory(node);
		node = next;
	}
	list->first = NULL;
	list->last = NULL;
	list->current = NULL;
	pthread_mutex_unlock(&list->mutex);
	return 0;
}

void audio_queue_init()
{
	cycleNodeBuffer audio_buf;
	audio_buf.max = 16;
	audio_buf.data = (audioBuffer *)calloc(audio_buf.max, sizeof(audioBuffer));
	if(audio_buf.data == NULL){
		err("audio buf calloc failed");
		return;
	}
	audio_buf.front = audio_buf.rear = 0;

}

void save_file()
{
	char buf[128] = {0};
	int wakeup_count;
	wakeup_count++;
	cycleNodeBuffer audio_buf;

	FILE *fpOrg;
	if(fpOrg){
		fclose(fpOrg);
		fpOrg = NULL;
	}
	fpOrg = fopen(buf, "w");
	if(fpOrg == NULL){
		err("%s open failed", buf);
		return;
	}
	while(audio_buf.rear != audio_buf.front){
		if(audio_buf.data[audio_buf.rear].buf){
			fwrite(audio_buf.data[audio_buf.rear].buf, 1, audio_buf.data[audio_buf.rear].len, fpOrg);
			free(audio_buf.data[audio_buf.rear].buf);
			audio_buf.data[audio_buf.rear].buf = NULL;
		}
		audio_buf.rear++;
		if(audio_buf.rear == audio_buf.max){
			audio_buf.rear = 0;
		}
	}
	audio_buf.rear = audio_buf.front = 0;
	fclose(fpOrg);
	fpOrg = NULL;
}

int player_add_job(play_job *job, const char *url)
{
	if(job == NULL || url == NULL){
		return -1;
	}
	int ret = 0;
	pthread_mutex_lock(&job->mutex);
	if(job->url){
		free(job->url);
	}
	int len = strlen(url);
	job->url = (char *)calloc(1, len+1);
	if(job->url == NULL){
		err("url calloc failed");
		pthread_mutex_unlock(&job->mutex);
		return -1;
	}
	strncpy(job->url, url, len);
	pthread_mutex_unlock(&job->mutex);
	pthread_cond_signal(&job->cond);
end:
	return ret;
}

void player_clean_job(play_job *job)
{
	if(job == NULL){
		return;
	}
	pthread_mutex_lock(&job->mutex);
	if(job->url){
		free(job->url);
		job->url = NULL;
	}
	pthread_mutex_unlock(&job->mutex);
}

void player_stop(player_ctrl *player)
{
	if(player == NULL){
		return;
	}
	player_clean_job(&player->job);
	player->pkt_queue->abort_request = 1;
}

void player_play(player_ctrl *player, const char *url)
{
	if(player == NULL || url == NULL){
		return;
	}
	player_stop(player);
	player_add_job(&player->job, url);
	pthread_cond_signal(&player->pkt_queue->cond);
}

void player_pause(player_ctrl *player)
{
	if(player == NULL){
		return;
	}
	player->pkt_queue->pause = 1;
}

void player_resume(player_ctrl *player)
{
	if(player == NULL){
		return;
	}
	player->pkt_queue->pause = 0;
}

#if 0
int player_add_task(player_ctrl *player, const char *url)
{
	if(player == NULL || player->list == NULL || url == NULL || strlen(url) <= 0){
		return -1;
	}
	play_list *node = (play_list *)calloc(1, sizeof(play_list));
	if(node == NULL){
		inf("play list calloc failed\n");
		return -1;;
	}
	node->url = (char *)malloc(strlen(url) + 1);
	if(node->url == NULL){
		inf("url malloc failed \n");
		free(node);
		return -1;
	}
	strcpy(node->url, url);
	play_list_ctrl *list = player->list;
	pthread_mutex_lock(&list->mutex);
	if(list->first == NULL){
		list->first = node;
	}
	if(list->last){
		list->last->next = node;
	}else{
		list->last = node;
	}
	list->last = list->last->next;
	pthread_mutex_unlock(&list->mutex);
	pthread_cond_signal(&list->cond);
	return 0;
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
	player_job_clean(player);
	player_task_stop(player);
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
	inf("%s\n", __func__);
	sem_destroy(&task->sem);
	free(task);
	return 0;
}
#endif

int play_start(player_ctrl *player, const char *url)
{
	if(player == NULL || url == NULL){
		return -1;
	}

	int err, stream_index;
	AVFormatContext *ic = NULL;
	ic = avformat_alloc_context();
	if(ic == NULL){
		inf("avformat alloc failed\n");
		return -1;
	}
	ic->interrupt_callback.callback = decode_interrupt_cb;
	ic->interrupt_callback.opaque = (void *)player->pkt_queue;
	inf("%s %d url: %s,ic: %p, &ic: %p, cb: %p, opaque: %p\n", __func__, __LINE__, url, ic, &ic, ic->interrupt_callback.callback, ic->interrupt_callback.opaque);
	err = avformat_open_input(&ic, url, NULL, NULL);
	if(err < 0){
		inf("%s open input failed: %d\n", url, err);
		return -1;
	}

	//if no this call, mp3 decoder report "Header missing"
	err = avformat_find_stream_info(ic, NULL);
	if(err < 0){
		inf("could not find codec parameters\n");
		return -1;
	}

	player->ic = ic;
	stream_component_open(player, 0);
	pthread_cond_signal(&player->job.cond);
}
#if 0
void *player_task_handle(void *param)
{
	player_ctrl *player = (player_ctrl *)param;
	play_list_ctrl *list = player->list;
	char *url = NULL;
	while(1){
		pthread_mutex_lock(&list->mutex);
		if(list->first == NULL && list->last == NULL){
			inf("%s wait...\n", __func__);
			pthread_cond_wait(&list->cond, &list->mutex);
		}
		if(list->current == NULL){
			list->current = list->first;
		}else{
			list->current = list->current->next;
		}
		if(list->current == NULL){
			pthread_mutex_unlock(&list->mutex);
			player_clean_task(list);
			continue;
		}
		url = (char *)calloc(1, strlen(list->current->url) + 1);
		if(url == NULL){
			inf("url calloc failed\n");
			exit(1);
		}
		inf("%s %d: %s\n", __func__, __LINE__, list->current->url);
		memcpy(url, list->current->url, strlen(list->current->url));
		pthread_mutex_unlock(&list->mutex);
		inf("%s url: %s\n", __func__, url);

		//new_task(player, url);
		free(url);
		task_wait(player->task);
		destory_task(player->task);
		player->task = NULL;
	}
}
#endif

int alsa_params_init(AudioParams *audio)
{
	audio->freq = alsa_output_sample_rate;
	audio->channels = alsa_output_channels;
	audio->fmt = alsa_output_fmt;
	return 0;
}

player_ctrl *player_init()
{
	//avdevice_register_all();
	avformat_network_init();
	av_register_all();

	player_ctrl *player = (player_ctrl *)calloc(1, sizeof(player_ctrl));
	if(player == NULL){
		inf("player calloc failed\n");
		return NULL;
	}
	pthread_mutex_init(&player->job.mutex, NULL);
	pthread_cond_init(&player->job.cond, NULL);
#if 0
	player->list = (play_list_ctrl *)calloc(1, sizeof(play_list_ctrl));
	if(player->list == NULL){
		inf("player list calloc failed\n");
		goto fail;
	}
#endif
	packetQueue *pq = create_packet_queue(PACKET_QUEUE_SIZE);
	frameQueue *fq = create_frame_queue(FRAME_QUEUE_SIZE, pq);
	player->pkt_queue = pq;
	player->frame_queue = fq;
	alsa_params_init(&player->audio_dst);
	pthread_t pt0, pt1, pt2, pt3;
	//pthread_create(&pt0, NULL, player_task_handle, (void *)player);
	pthread_create(&pt2, NULL, read_thread, (void *)player);
#if PROD_FLAG
	pthread_create(&pt1, NULL, pcm_write_thread, (void *)player);
	pthread_create(&pt3, NULL, decode_thread, (void *)player);
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
		inf("filename: %s\n", filename);
	}else{
		err("usage: ./player fileName");
		return -1;
	}
	player_ctrl *player = player_init();
	sleep(1);
	player_play(player, argv[1]);
#if 1
	char buf[64];
	while(1){
		err("cli>");
		memset(buf, 0, sizeof(buf));
		scanf("%s", buf);
		err("get: %s\n", buf);
		if(strcmp(buf, "pause") == 0){
			player_pause(player);
		}else if(strcmp(buf, "resume") == 0){
			player_resume(player);
		}else if(strcmp(buf, "stop") == 0){
			player_stop(player);
		}else{
			player_play(player, buf);
		}
	}
#endif

	while(1);
	return 0;
}
