#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mtcp_server.h"

#define SYN 0
#define SYNACK 1
#define FIN 2
#define FINACK 3
#define ACK 4
#define DATA 5

#define RECV_BUF_SIZE 268435456

typedef enum {INIT,HS3,RW,HS4,END} state_t;			
typedef struct
{
	int front,rear;
	int capacity;
	unsigned char *array;
}buffer_t;
/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t recvbuf_mutex = PTHREAD_MUTEX_INITIALIZER;

/* The Sending Thread and Receive Thread Function */
static void *send_thread();
static void *receive_thread();


/* Receive buffer circular queue functions */
buffer_t *create_buffer(int size)
{
	buffer_t *q=(buffer_t *)malloc(sizeof(buffer_t));
	if(!q)return NULL;
	q->capacity = size;
	q->front = -1;
	q->rear = -1;
	q->array=(unsigned char *)malloc(q->capacity*sizeof(unsigned char));
	if(!q->array)return NULL;
	return q;
} 

int is_empty(buffer_t *q)
{
	return (q->front==-1);
}

int is_full(buffer_t *q)
{
	return (((q->rear+1)%q->capacity) == q->rear);
}

int buf_size(buffer_t *q)
{
	return (q->capacity - q->rear + q->front +1)%q->capacity;
}

int enqueeue(buffer_t *q, unsigned char *src,  int len)
{
	if(is_full(q))return 0;
	else if(len > buf_size(q))return 0;
	else
	{
		q->rear = (q->rear + len)%q->capacity;
		memcpy(&q->array[q->rear],src,len);
		if(q->front==-1)
		{
			q->front = q->rear;
		}
		return 1
	}
}

int dequeue(buffer_t *q,unsigned char *dst, int len)
{
	if(is_empty(q))return 0;
	else
	{
		memcpy(dst,&q->array[q->front],len);
		if(q->front==q->rear)
			q->front=q->rear=-1;
		else
			q->front=(q->front + len)%q->capacity;
		return 1;
	}
}

void mtcp_accept(int socket_fd, struct sockaddr_in *server_addr){

}

int mtcp_read(int socket_fd, unsigned char *buf, int buf_len){

}

void mtcp_close(int socket_fd){

}

void create_packet(unsigned char *packet, unsigned char type, unsigned int seq, unsigned char *data, size_t data_len)
{
	memset(packet,0,MAX_BUF_SIZE+4);
	unsigned int header = htonl(((type & 0xf)<<28) | (seq & 0x0fffffff));
	*((unsigned int *)packet) = header;
	if(data)
		memcpy(packet+4,data,data_len);
}

unsigned char get_packet_type(unsigned char *packet)
{
	unsigned int header = ntohl(*((unsigned int *)packet));
	return (header>>28)&0xf;
}

unsigned int get_packet_seq(unsigned char *packet)
{
	unsigned int header = ntohl(*((unsigned int *)packet));
	return header & 0x0fffffff;
}

static void *send_thread(){

}

static void *receive_thread(){

}
