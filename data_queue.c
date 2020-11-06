#include "data_queue.h"

void destory_MyAVPacketList(pMyAVPacketList node)
{
	if(node){
		free(node);
	}
}

packetQueue *create_packet_queue(void);
{
	packetQueue *q = (packetQueue *)calloc(1, sizeof(packetQueue));
	if(q == NULL){
		log_print("calloc failed\n");
		return NULL;
	}
	q->first_pkt = q->last_pkt = NULL;
	pthread_mutex_lock(&q->mutex);
	pthread_cond_init(&q->cond);
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
	pthread_mutex_destory(&q->mutex);
	pthread_cond_destory(&q->cond);
}

frameQueue *create_frame_queue(int max, packetQueue *pkt_queue)
{
	frameQueue *q = (frameQueue *)calloc(1, sizeof(frameQueue));
	if(q == NULL){
		log_print("calloc failed\n");
		return NULL;
	}
	q->max = max;
	q->queue = (Frame *)calloc(max, sizeof(Frame));
	if(q->queue == NULL){
		log_queue("calloc failed\n");
		return NULL;
	}
	q->size = 0;
	q->read_index = q->write_index = 0;
	q->pkt_queue = pkt_queue;
	pthread_mutex_lock(&q->mutex);
	pthread_cond_init(&q->cond);
	return q;
}

void clean_frame_queue(frameQueue *q)
{
	if(q == NULL){
		return;
	}
	pthread_mutex_lock(&q->mutex);
	if(q->queue){
		free(q->queue);
	}
	q->size = 0;
	q->read_index = q->write_index = 0;
	pthread_mutex_unlock(&q->mutex);
}

void destory_frame_queue(frameQueue *q)
{
	clean_frame_queue(q);
	pthread_mutex_destory(&q->mutex);
	pthread_cond_destory(&q->cond);
}
