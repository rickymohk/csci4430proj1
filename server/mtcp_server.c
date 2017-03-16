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

#define MAX_BUF_SIZE 1024
#define RECV_BUF_SIZE 268435456

typedef enum {INIT,HS3,RW,HS4,END} state_t;			
typedef struct
{
	int front,rear;
	int capacity;
	unsigned char *array;
}buffer_t;

/* -------------------- Global Variables -------------------- */
int sockfd;
struct sockaddr_in *addr;

state_t state;
unsigned int current_ack;
unsigned char last_recv_type;
unsigned char last_sent_type;
ssize_t recvfrom_err = 0;

buffer_t *recvbuf;

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

int enqueue(buffer_t *q, unsigned char *src,  int len)
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
		return 1;
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

void mtcp_accept(int socket_fd, struct sockaddr_in *client_addr){
	srand((unsigned)time(NULL));
	current_ack = rand() & 0x0fffffff;
	sockfd = socket_fd;
	addr = client_addr;
	state = INIT;
	recvbuf = create_buffer(RECV_BUF_SIZE);

	if(!recvbuf)
	{
		perror("cannot create send buffer");
		exit(1);
	}

	if(pthread_create(&send_thread_pid,NULL,send_thread,NULL)!=0)
	{
		perror("cannot create send thread");
		exit(1);
	}

	if(pthread_create(&recv_thread_pid,NULL,receive_thread,NULL)!=0)
	{
		perror("cannot create receive thread");
		exit(1);
	}

	//change state to 3-way handshake
	pthread_mutex_lock(&info_mutex);
	state = HS3;													
	pthread_mutex_unlock(&info_mutex);

	//wait until wake signal from send thread
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig,&app_thread_sig_mutex);
	pthread_mutex_unlock(&app_thread_sig_mutex);

	//change state to read/write
	pthread_mutex_lock(&info_mutex);
	state = RW;													
	pthread_mutex_unlock(&info_mutex);

	return;
}
int mtcp_read(int socket_fd, unsigned char *buf, int buf_len){
	if(state==HS4)return 0;
	if(recvfrom_err==-1)return -1;
	//wake send thread in case it is waiting
	pthread_mutex_lock(&send_thread_sig);
	pthread_cond_signal(&send_thread_sig);
	pthread_mutex_unlock(&send_thread_sig);

	//wait for send thread wake signal
	pthread_mutex_lock(&app_thread_sig);
	pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
	pthread_mutex_unlock(&app_thread_sig);

	return buf_len;

}

void mtcp_close(int socket_fd){

	pthread_mutex_lock(&info_mutex);
	state = END;
	pthread_mutex_unlock(&info_mutex);
	pthread_join(recv_thread_pid,NULL);
	pthread_join(send_thread_pid,NULL);
	close(socket_fd);

	return;
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
	unsigned char packet[MAX_BUF_SIZE+4];
	int len;
	unsigned int last_ack;
	unsigned int seq;					
	state_t current_state;
	unsigned char last_type = 0xff;
	unsigned char sent_type = 0xff;
	ssize_t sendto_retv = 0;
	do
	{
		//Sleep						
		pthread_mutex_lock(&send_thread_sig_mutex);
		pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);
		pthread_mutex_unlock(&send_thread_sig_mutex);
		
		//Check state
		pthread_mutex_lock(&info_mutex);
		last_type = last_recv_type;
		current_state = state;
		seq = get_packet_seq;
		pthread_mutex_unlock(&info_mutex);

		if(current_state==HS3)
		{
			if(last_type==0xff)
			{
				//Send SYNACK
				sent_type = SYNACK;
				create_packet(packet,SYNACK,seq,NULL,0);
				sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(addr));

				pthread_mutex_lock(&send_thread_sig_mutex);
				pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);
				pthread_mutex_unlock(&send_thread_sig_mutex);

				
			}
			else if(last_type==ACK)
			{				
				//Wake app thread cause mtcp_accept() return
				pthread_mutex_lock(&app_thread_sig_mutex);
				pthread_cond_signal(&app_thread_sig);	
				pthread_mutex_unlock(&app_thread_sig_mutex);	
			}

		}
		else if(current_state==RW)
		{
			if((last_type==ACK) || (last_type==DATA))
			{
				sent_type = ACK;
				create_packet(packet,ACK,seq,NULL,0);
				sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(addr));
				pthread_mutex_lock(&app_thread_sig_mutex);
				pthread_cond_signal(&app_thread_sig);	
				pthread_mutex_unlock(&app_thread_sig_mutex);
			}
		}
		else if(current_state==HS4)
		{
			sent_type = FINACK;
			create_packet(packet,FINACK,seq,NULL,0);
			sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(addr));
		}

	}(current_state!=END);

	pthread_exit(NULL);
}

static void *receive_thread(){
	unsigned char packet[MAX_BUF_SIZE+4];
	unsigned char data[MAX_BUF_SIZE];
	size_t len;
	unsigned char last_type = 0xff;
	unsigned char current_type = 0xff;
	unsigned int seq;
	state_t current_state;

	do
	{
		//Monitor Socket
		len = recvfrom(sockfd,(void *)packet,MAX_BUF_SIZE+4,0,NULL,NULL);
		current_type = get_packet_type(packet);
		
		//Check & update state
		pthread_mutex_lock(&info_mutex);
		current_state = state;
		last_recv_type = current_type;
		last_type = last_sent_type;
		seq = get_packet_seq(packet);
		current_ack = get_packet_ack(packet);

		pthread_mutex_unlock(&info_mutex);
		
		//wake send thread
		if((current_type==SYN)||(last_type==ACK && current_type==DATA))
		{
			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_signal(&send_thread_sig);
			pthread_mutex_unlock(&send_thread_sig_mutex);
			memcpy(data[seq], packet[4], len);
		}
		else if(current_type==FIN)
		{
			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_signal(&send_thread_sig);
			pthread_mutex_unlock(&send_thread_sig_mutex);
		}

	}while(last_type!=FINACK && current_type!=ACK);

	pthread_exit(NULL);
}
