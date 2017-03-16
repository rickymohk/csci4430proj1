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

#define MAX_BUF_SIZE 1024
#define SEND_BUF_SIZE 268435456

#define DEBUG 1

typedef enum {INIT,HS3,RW,HS4,END,NIL} state_t;			
typedef struct
{
	int front,rear;
	int capacity;
	unsigned char *array;
}buffer_t;

/* -------------------- Global Variables -------------------- */
int sockfd;
struct sockaddr_in *addr;

state_t state = NIL;
unsigned int current_ack;
unsigned char last_recv_type=-1;
unsigned char last_sent_type=-1;
ssize_t sendto_err = 0;

buffer_t *sendbuf;

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sendbuf_mutex = PTHREAD_MUTEX_INITIALIZER;

/* The Sending Thread and Receive Thread Function */
static void *send_thread();
static void *receive_thread();


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

/* Connect Function Call (mtcp Version) */
void mtcp_connect(int socket_fd, struct sockaddr_in *server_addr){
	srand((unsigned)time(NULL));
	current_ack = rand() & 0x0fffffff;
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
	pthread_mutex_lock(&info_mutex);
	state = HS3;													
	pthread_mutex_unlock(&info_mutex);
	
	//wake send thread in case it is waiting
	pthread_mutex_lock(&send_thread_sig_mutex);
	pthread_cond_signal(&send_thread_sig);
	pthread_mutex_unlock(&send_thread_sig_mutex);		
	
	//wait for send thread finish 3-way handshake
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig,&app_thread_sig_mutex);
	pthread_mutex_unlock(&app_thread_sig_mutex);	
	
	//change state to read/write
	pthread_mutex_lock(&info_mutex);
	state = RW;													
	pthread_mutex_unlock(&info_mutex);	
	return;
}

/* Write Function Call (mtcp Version) */
int mtcp_write(int socket_fd, unsigned char *buf, int buf_len){
	if(sendto_err==-1 || state==HS4 || state==NIL || state==END)return -1;
	pthread_mutex_lock(&sendbuf_mutex);
	int retv = enqueue(sendbuf,buf,buf_len);
	pthread_mutex_unlock(&sendbuf_mutex);
	
	if(retv)
	{
		pthread_mutex_lock(&send_thread_sig_mutex);
		pthread_cond_signal(&send_thread_sig);		//wake send thread
		pthread_mutex_unlock(&send_thread_sig_mutex);			
	}
	else
	{
		return 0;			//buffer full
	}

	return buf_len;
}

/* Close Function Call (mtcp Version) */
void mtcp_close(int socket_fd){
	//change state to 4-way handshake
	if(state!=END)
	{
		pthread_mutex_lock(&info_mutex);
		state = HS4;						
		pthread_mutex_unlock(&info_mutex);
		//wake send thread
		pthread_mutex_lock(&send_thread_sig_mutex);
		pthread_cond_signal(&send_thread_sig);
		pthread_mutex_unlock(&send_thread_sig_mutex);	
		//wait for send thread finish 4-way handshake
		pthread_mutex_lock(&app_thread_sig_mutex);
		pthread_cond_wait(&app_thread_sig,&app_thread_sig_mutex);	//wait for send thread
		pthread_mutex_unlock(&app_thread_sig_mutex);	
		
		state = END;
		pthread_join(recv_thread_pid,NULL);
		pthread_join(send_thread_pid,NULL);
		close(socket_fd);	
		
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
	if(DEBUG)printf("send_thread created\n");
	unsigned char packet[MAX_BUF_SIZE+4];
	unsigned char data[MAX_BUF_SIZE];
	int len;
	unsigned int last_ack;
	unsigned int seq;					
	struct timespec abstime;
	state_t current_state;
	unsigned char last_type = -1;
	unsigned char sent_type = -1;
	ssize_t sendto_retv = 0;
	do
	{
		//Sleep
		if(DEBUG)printf("sleep for 1 sec\n");
		clock_gettime(CLOCK_REALTIME,&abstime);
		abstime.tv_sec++;							
		pthread_mutex_lock(&send_thread_sig_mutex);
		pthread_cond_timedwait(&send_thread_sig,&send_thread_sig_mutex,&abstime);
		pthread_mutex_unlock(&send_thread_sig_mutex);
		
		//Check state
		last_ack = seq;
		pthread_mutex_lock(&info_mutex);
		last_type = last_recv_type;
		current_state = state;
		seq = current_ack;
		pthread_mutex_unlock(&info_mutex);
		//Send packet
		if(DEBUG)printf("current_state: %d\n",current_state);
		if(current_state==HS3)
		{
			if(DEBUG)printf("HS3 state\n");
			if(last_type==(unsigned char)-1)
			{
				if(DEBUG)printf("send SYN\n");
				//Send SYN
				sent_type = SYN;
				create_packet(packet,SYN,seq,NULL,0);
				sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(struct sockaddr));
				if(DEBUG)printf("sento_retv=%d,errno=%d\n",sendto_retv,errno);

			}
			else if(last_type==SYNACK)
			{
				if(DEBUG)printf("send ACK after SYNACK\n");
				//Send ACK
				sent_type = ACK;
				create_packet(packet,ACK,seq,NULL,0);
				sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(struct sockaddr));
				
				//Wake app thread cause mtcp_connect() return
				pthread_mutex_lock(&app_thread_sig_mutex);
				pthread_cond_signal(&app_thread_sig);	
				pthread_mutex_unlock(&app_thread_sig_mutex);					
			}
		}
		else if(current_state==RW)
		{
			if( (last_type==SYNACK) || ((last_type==ACK) && (last_ack!=seq)) )	//first packet or new packet
			{
				if(DEBUG)printf("send DATA\n");
				//Fetch new packet from buffer
				if(!is_empty(sendbuf))			//have data to send
				{
					pthread_mutex_lock(&sendbuf_mutex);
					len = buf_size(sendbuf)>1000?1000:buf_size(sendbuf);
					if(dequeue(sendbuf,data,len))
					{
						sent_type = DATA;
						create_packet(packet,DATA,seq,data,len);
						sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(struct sockaddr));		
					}
					pthread_mutex_unlock(&sendbuf_mutex);
				}
				else			//buffer empty, sleep until app thread mtcp_write() called
				{	
					pthread_mutex_lock(&send_thread_sig_mutex);
					pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);		
					pthread_mutex_unlock(&send_thread_sig_mutex);
				}
			}
			else
			{
				//Retransmit
				if(DEBUG)printf("Retransmit\n");
				sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(struct sockaddr));
			}
				
		}
		else if(current_state==HS4)
		{
			if(last_type!=FINACK)
			{
				if(DEBUG)printf("send FIN\n");
				//Send FIN
				sent_type = FIN;
				create_packet(packet,FIN,seq,NULL,0);	
				sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(struct sockaddr));	
			}
			else
			{
				if(DEBUG)printf("last ACK\n");
				//Send ACK
				sent_type = ACK;
				create_packet(packet,ACK,seq,NULL,0);	
				sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)addr,sizeof(struct sockaddr));	
				
				//Wake app thread cause mtcp_close() return
				pthread_mutex_lock(&app_thread_sig_mutex);
				pthread_cond_signal(&app_thread_sig);		
				pthread_mutex_unlock(&app_thread_sig_mutex);		
				
			}
			
		}
		
		//Update state
		pthread_mutex_lock(&info_mutex);
		sendto_err = sendto_retv;
		last_sent_type = sent_type;
		pthread_mutex_unlock(&info_mutex);	
	}while(!(current_state==HS4 && sent_type==ACK));
	
	pthread_exit(NULL);
}

static void *receive_thread(){
	if(DEBUG)printf("receive_thread created\n");
	unsigned char packet[MAX_BUF_SIZE+4];
	ssize_t len;
	unsigned char last_type = -1;
	unsigned char current_type = -1;
	state_t current_state;
	do
	{
		//Monitor Socket
		len = recvfrom(sockfd,(void *)packet,MAX_BUF_SIZE+4,0,NULL,NULL);
		current_type = get_packet_type(packet);
		if(DEBUG)printf("received packet type: %d\n",current_type);
		//Check & update state
		pthread_mutex_lock(&info_mutex);
		current_state = state;
		last_recv_type = current_type;
		last_type = last_sent_type;
		current_ack = get_packet_ack(packet);
		pthread_mutex_unlock(&info_mutex);
		
		//wake send thread
		if((last_type==SYN && current_type==SYNACK)||(last_type==DATA && current_type==ACK)||(last_type==FIN && current_type==FINACK))
		{
			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_signal(&send_thread_sig);
			pthread_mutex_unlock(&send_thread_sig_mutex);				
		}
	}while(current_type!=FINACK);

	pthread_exit(NULL);
}













