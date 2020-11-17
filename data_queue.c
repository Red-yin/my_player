#include "data_queue.h"
#include "log.h"

void destory_MyAVPacketList(pMyAVPacketList node)
{
	if(node){
		free(node);
	}
}

packetQueue *create_packet_queue(void)
{
	packetQueue *q = (packetQueue *)calloc(1, sizeof(packetQueue));
	if(q == NULL){
		log_print("calloc failed\n");
		return NULL;
	}
	q->first_pkt = q->last_pkt = NULL;
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->cond, NULL);
	return q;
}

void clean_packet_queue(packetQueue *q)
{
	if(q == NULL){
		return;
	}
	pthread_mutex_lock(&q->mutex);
	pMyAVPacketList node = q->first_pkt;
	pMyAVPacketList next = NULL;
	while(node){
		next = node->next;
		destory_MyAVPacketList(node);
		node = next;
	}
	q->first_pkt = q->last_pkt = NULL;
	q->nb_packets = 0;
	q->size = 0;
	pthread_mutex_unlock(&q->mutex);
}

void destory_packet_queue(packetQueue *q)
{
	clean_packet_queue(q);
	pthread_mutex_destroy(&q->mutex);
	pthread_cond_destroy(&q->cond);
}

int packet_queue_put(packetQueue *q, AVPacket *pkt)
{
	if(q == NULL || pkt == NULL){
		return -1;
	}
	pMyAVPacketList pkt1 = (pMyAVPacketList)calloc(1, sizeof(MyAVPacketList));
	if(pkt1 == NULL){
		log_print("[%s %d]calloc failed\n", __FILE__, __LINE__);
		return -1;
	}
	pkt1->pkt = *pkt;
	pkt1->next = NULL;
	pthread_mutex_lock(&q->mutex);
	if(q->abort_request){
		pthread_mutex_unlock(&q->mutex);
		free(pkt1);
		return -1;
	}

	if(q->last_pkt){
		q->last_pkt->next = pkt1;
	}else{
		q->first_pkt = pkt1;
	}
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size + sizeof(*pkt1);
	pthread_mutex_unlock(&q->mutex);
	pthread_cond_signal(&q->cond);
	return 0;
}

int packet_queue_get(packetQueue *q, AVPacket *pkt, int block)
{
	int ret;
	pMyAVPacketList pkt1;
	if(q == NULL || pkt == NULL){
		return -1;
	}
	pthread_mutex_lock(&q->mutex);
	for(;;){
		if(q->abort_request){
			ret = -1;
			break;
		}
		pkt1 = q->first_pkt;
		if(pkt1){
			*pkt = pkt1->pkt;
			q->first_pkt = pkt1->next;
			if(q->first_pkt == NULL){
				q->last_pkt = NULL;
			}
			q->nb_packets--;
			q->size -= pkt1->pkt.size + sizeof(*pkt1);
			free(pkt1);
			ret = 1;
			break;
		}else if(block){
			pthread_cond_wait(&q->cond, &q->mutex);
		}else{
			ret = 0;
			break;
		}
	}
	pthread_mutex_unlock(&q->mutex);
	return ret;
}

int packet_queue_signal(packetQueue *q)
{
	if(q == NULL){
		return -1;
	}
	pthread_cond_signal(&q->cond);
	return 0;
}

frameQueue *create_frame_queue(int max, packetQueue *pkt_queue)
{
	int i;
	frameQueue *q = (frameQueue *)calloc(1, sizeof(frameQueue));
	if(q == NULL){
		log_print("calloc failed\n");
		return NULL;
	}
	q->max = max;
	q->queue = (Frame *)calloc(max, sizeof(Frame));
	if(q->queue == NULL){
		log_print("calloc failed\n");
		return NULL;
	}
	for(i = 0; i < max; i++){
		if(!(q->queue[i].frame = av_frame_alloc())){
			log_print("av frame calloc failed\n");
			return NULL;
		}
	}

	q->size = 0;
	q->read_index = q->write_index = 0;
	q->pkt_queue = pkt_queue;
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->cond, NULL);
	return q;
}

void clean_frame_queue(frameQueue *q)
{
	if(q == NULL){
		return;
	}
	int i;
	pthread_mutex_lock(&q->mutex);
	if(q->queue){
		for(i = q->read_index; i != q->write_index; i++){
			i = i == q->max ? 0: i;
			av_frame_unref(q->queue[i].frame);
		}
	}
	q->size = 0;
	q->read_index = q->write_index = 0;
	pthread_mutex_unlock(&q->mutex);
}

void destory_frame_queue(frameQueue *q)
{
	int i;
	clean_frame_queue(q);
	pthread_mutex_lock(&q->mutex);
	if(q->queue){
		for(i = 0; i < q->max; i++){
			av_frame_free(&q->queue[i].frame);
		}
		free(q->queue);
	}
	pthread_mutex_unlock(&q->mutex);

	pthread_mutex_destroy(&q->mutex);
	pthread_cond_destroy(&q->cond);
}

int frame_queue_put(frameQueue *q, AVFrame *frame, int block)
{
	if(q == NULL || frame == NULL){
		return -1;
	}
	if(q->pkt_queue && q->pkt_queue->abort_request){
		return -1;
	}
	pthread_mutex_lock(&q->mutex);
	while(q->size >= q->max){
		if(block)
			pthread_cond_wait(&q->cond, &q->mutex);
		else{
			pthread_mutex_unlock(&q->mutex);
			return -1;
		}
		if(q->pkt_queue && q->pkt_queue->abort_request){
			pthread_mutex_unlock(&q->mutex);
			return -1;
		}
	}
	av_frame_move_ref(q->queue[q->write_index].frame, frame);
	q->write_index++;
	q->write_index = q->write_index == q->max ? 0: q->write_index;
	q->size++;
	pthread_mutex_unlock(&q->mutex);
	pthread_cond_signal(&q->cond);
	return 0;
}

int frame_queue_get(frameQueue *q, AVFrame *frame, int block)
{
	if(q == NULL || frame == NULL){
		return -1;
	}
	pthread_mutex_lock(&q->mutex);
	if(q->pkt_queue && q->pkt_queue->abort_request){
		return -1;
	}
	while(q->size <= 0){
		if(q->pkt_queue && q->pkt_queue->abort_request || q->pkt_queue->eof){
			pthread_mutex_unlock(&q->mutex);
			return -1;
		}
		if(block)
			pthread_cond_wait(&q->cond, &q->mutex);
		else{
			pthread_mutex_unlock(&q->mutex);
			return 1;
		}
	}
	av_frame_move_ref(frame, q->queue[q->read_index].frame);
	av_frame_unref(q->queue[q->read_index].frame);
	q->read_index++;
	q->read_index = q->read_index == q->max ? 0: q->read_index;
	q->size--;
	pthread_mutex_unlock(&q->mutex);

	return 0;
}

int frame_queue_set_pkt_queue(frameQueue *q, packetQueue *pkt_queue)
{
	if(q == NULL || pkt_queue == NULL){
		return -1;
	}

	pthread_mutex_lock(&q->mutex);
	q->pkt_queue = pkt_queue;
	pthread_mutex_unlock(&q->mutex);
	return 0;
}
