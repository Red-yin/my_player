
#include <pthread.h>
#include <semaphore.h>
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
typedef struct MyAVPacketList{
	AVPacket pkt;
	struct MyAVPacketList *next;
}MyAVPacketList, *pMyAVPacketList;

typedef struct packetQueue{
	pMyAVPacketList first_pkt, last_pkt;
	int nb_packets;
	int size;
	int max;
	int abort_request;
	int pause;
	int eof;
	sem_t stop_sem;
	sem_t start_sem;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
}packetQueue;

typedef struct Frame{
	AVFrame *frame;
}Frame;

typedef struct frameQueue{
	Frame *queue;
	int size;
	int max;
	int read_index;
	int write_index;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	packetQueue *pkt_queue;
}frameQueue;

packetQueue *create_packet_queue(int max);
void clean_packet_queue(packetQueue *pkt_queue);
void destory_packet_queue(packetQueue *pkt_queue);
//copy src data which is saved in q to pkt, pkt must have enough memory
int packet_queue_get(packetQueue *q, AVPacket *pkt, int block);
int packet_queue_put(packetQueue *q, AVPacket *pkt, int block);
int packet_queue_signal(packetQueue *q);


frameQueue *create_frame_queue(int max, packetQueue *pkt_queue);
void clean_frame_queue(frameQueue *frame_queue);
void destory_frame_queue(frameQueue *frame_queue);
int frame_queue_put(frameQueue *q, AVFrame *frame, int block);
int frame_queue_get(frameQueue *q, AVFrame *frame, int block);
int frame_queue_set_pkt_queue(frameQueue *q, packetQueue *pkt_queue);
int frame_queue_signal(frameQueue *q);
