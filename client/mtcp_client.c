#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "mtcp_client.h"

#define SYN 0
#define SYNACK 1
#define FIN 2
#define FINACK 3
#define ACK 4
#define DATA 5

#define SEND_BUF_SIZE 268435456

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
buffer_t sendbuf;

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Send buffer circular queue functions */
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

/* Connect Function Call (mtcp Version) */
void mtcp_connect(int socket_fd, struct sockaddr_in *server_addr){
	srand((unsigned)time(NULL));
	sockfd = socket_fd;
	addr = server_addr;
	state = INIT;
	sendbuf = create_buffer(SEND_BUF_SIZE);
	if(!sendbuf)
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
	pthread_mutex_lock(&state_mutex);
	state = HS3;													
	pthread_mutex_unlock(&state_mutex);
	//wake send thread
	pthread_mutex_lock(&send_thread_sig_mutex);
	pthread_cond_signal(&send_thread_sig,&send_thread_sig_mutex);
	pthread_mutex_unlock(&send_thread_sig_mutex);		
	//wait for send thread finish 3-way handshake
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig,&app_thread_sig_mutex);	//wait for send thread
	pthread_mutex_unlock(&app_thread_sig_mutex);	
	
	return;
}

/* Write Function Call (mtcp Version) */
int mtcp_write(int socket_fd, unsigned char *buf, int buf_len){
	if(state==HS4)return 0;
	if(state==END)return -1;
	if(!enqueeue(sendbuf,buf,buf_len))return -1;
	
	pthread_mutex_lock(&send_thread_sig_mutex);
	pthread_cond_signal(&send_thread_sig,&send_thread_sig_mutex);		//wake send thread
	pthread_mutex_unlock(&send_thread_sig_mutex);	
	return buf_len;
}

/* Close Function Call (mtcp Version) */
void mtcp_close(int socket_fd){
	//change state to 4-way handshake
	if(state!=END)
	{
		pthread_mutex_lock(&state_mutex);
		state = HS4;						
		pthread_mutex_unlock(&state_mutex);
		//wake send thread
		pthread_mutex_lock(&send_thread_sig_mutex);
		pthread_cond_signal(&send_thread_sig,&send_thread_sig_mutex);
		pthread_mutex_unlock(&send_thread_sig_mutex);	
		//wait for send thread finish 4-way handshake
		pthread_mutex_lock(&app_thread_sig_mutex);
		pthread_cond_wait(&app_thread_sig,&app_thread_sig_mutex);	//wait for send thread
		pthread_mutex_unlock(&app_thread_sig_mutex);	
		
		pthread_join(send_thread_pid,NULL);
		pthread_join(recv_thread_pid,NULL);
		close(socket_fd);	
		state = END;
	}

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

unsigned int get_packet_ack(unsigned char *packet)
{
	unsigned int header = ntohl(*((unsigned int *)packet));
	return header & 0x0fffffff;
}

static void *send_thread(){
	unsigned char packet[MAX_BUF_SIZE+4];
	unsigned char data[MAX_BUF_SIZE];
	int len;
//-------------------------------------Connect----------------------------------------------------
	pthread_mutex_lock(&send_thread_sig_mutex);
	while(state!=HS3)												//wait until 3-way handshake initiated
	{
		pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);	//wait for app thread
	}
	pthread_mutex_unlock(&send_thread_sig_mutex);		
	
	//3-way handshake starts 
	unsigned int seq = rand();					//initialize sequence number
	struct timespect abstime;
	//send a SYN
	create_packet(packet,SYN,seq,NULL,0);
	sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));
	
	clock_gettime(CLOCK_REALTIME,&abstime);
	abstime.tv_sec++;							//generate timespec for timedwait
	
	pthread_mutex_lock(&send_thread_sig_mutex);
	while(pthread_cond_timedwait(&send_thread_sig,&send_thread_sig_mutex,&abstime)==ETIMEDOUT)	//wait for recv thread
	{
		clock_gettime(CLOCK_REALTIME,&abstime);
		abstime.tv_sec++;			
		sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));	//timeout, resend SYN
	}
	pthread_mutex_unlock(&send_thread_sig_mutex);	
	//send a ACK
	seq = current_ack;
	create_packet(packet,ACK,seq,NULL,0);
	sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));
	
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_signal(&app_thread_sig,&app_thread_sig_mutex);		//wake up app thread cause mtcp_connect() return
	pthread_mutex_unlock(&app_thread_sig_mutex);	
	
	
//----------------------------------------Read/Write---------------------------------------	
	//Go into data transfer state
	pthread_mutex_lock(&state_mutex);
	state = RW;						
	pthread_mutex_unlock(&state_mutex);
	//wait for app thread write/close
	pthread_mutex_lock(&send_thread_sig_mutex);
	while(state!=HS4)				//break loop if 4-way handshake initiated					
	{
		if(!is_empty(sendbuf))			//have data to send
		{
			len = buf_size(send_buf)>1000?1000:buf_size(send_buf);
			if(dequeue(sendbuf,data,len))
			{
				seq = current_ack;
				create_packet(packet,DATA,seq,data,len);
				clock_gettime(CLOCK_REALTIME,&abstime);
				abstime.tv_sec++;							//generate timespec for timedwait			
				while(pthread_cond_timedwait(&send_thread_sig,&send_thread_sig_mutex,&abstime)==ETIMEDOUT)	//wait for recv thread
				{
					clock_gettime(CLOCK_REALTIME,&abstime);
					abstime.tv_sec++;	
					sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));	//timeout, resend SYN
				}				
			}
		}
		else			//buffer empty
		{			
			pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);	//wait for app thread			
		}
	}
	pthread_mutex_unlock(&send_thread_sig_mutex);				
	
	
	
//-------------------------------------------Close---------------------------------------
	//4-way handshake starts
	//send FIN
	seq = current_ack;
	create_packet(packet,FIN,seq,NULL,0);
	sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));	
	clock_gettime(CLOCK_REALTIME,&abstime);
	abstime.tv_sec++;														//generate timespec for timedwait	
	pthread_mutex_lock(&send_thread_sig_mutex);
	while(pthread_cond_timedwait(&send_thread_sig,&send_thread_sig_mutex,&abstime)==ETIMEDOUT)	//wait for recv thread
	{
		clock_gettime(CLOCK_REALTIME,&abstime);
		abstime.tv_sec++;	
		sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));	//timeout, resend FIN
	}
	pthread_mutex_unlock(&send_thread_sig_mutex);	
	//send ACK
	seq = current_ack;
	create_packet(packet,ACK,seq,NULL,0);
	sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));
	
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_signal(&app_thread_sig,&app_thread_sig_mutex);		//wake up app thread cause mtcp_close() return
	pthread_mutex_unlock(&app_thread_sig_mutex);		
	
	pthread_exit(NULL);
}

static void *receive_thread(){
	unsigned char packet[MAX_BUF_SIZE+4];
	size_t len;
//----------------------------------Connect---------------------------------------
	//Monitor for SYNACK
	do
	{
		len = recvfrom(sockfd,(void *)packet,MAX_BUF_SIZE+4,NULL,(struct sockaddr *)addr,sizeof(addr));		
	}while(get_packet_type(packet)!=SYNACK);

	//SYNACK received
	pthread_mutex_lock(&info_mutex);
	current_ack = get_packet_ack(packet);					//get ack number from recv packet
	pthread_mutex_unlock(&info_mutex);
	
	pthread_mutex_lock(&send_thread_sig_mutex);
	pthread_cond_signal(&send_thread_sig,&app_thread_sig_mutex);		//wake send thread
	pthread_mutex_unlock(&send_thread_sig_mutex);


//------------------------------Read/Write----------------------------------------------	
	//Monitor for ACK and FINACK
	do
	{
		len = recvfrom(sockfd,(void *)packet,MAX_BUF_SIZE+4,NULL,(struct sockaddr *)addr,sizeof(addr));
		if(get_packet_type(packet)==ACK)
		{
			pthread_mutex_lock(&info_mutex);
			current_ack = get_packet_ack(packet);					//get ack number from recv packet
			pthread_mutex_unlock(&info_mutex);
			
			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_signal(&send_thread_sig,&app_thread_sig_mutex);		//wake send thread
			pthread_mutex_unlock(&send_thread_sig_mutex);			
		}
	}while(get_packet_type(packet)!=FINACK);
	
	
//------------------------------Close----------------------------------------------------
	//FINACK received
	pthread_mutex_lock(&info_mutex);
	current_ack = get_packet_ack(packet);					//get ack number from recv packet
	pthread_mutex_unlock(&info_mutex);
	
	pthread_mutex_lock(&send_thread_sig_mutex);
	pthread_cond_signal(&send_thread_sig,&app_thread_sig_mutex);		//wake send thread
	pthread_mutex_unlock(&send_thread_sig_mutex);	
	
	pthread_exit(NULL);
}













