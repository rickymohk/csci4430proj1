#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include "mtcp_server.h"

#define SYN 0
#define SYNACK 1
#define FIN 2
#define FINACK 3
#define ACK 4
#define DATA 5

#define MAX_BUF_SIZE 1024
#define RECV_BUF_SIZE 268435456

#define DEBUG 0

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
socklen_t addrlen;
struct sockaddr_in *client;
state_t state=NIL;
unsigned int current_ack;
unsigned int last_ack;
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
	if(q->front==-1)return 0;
	return (q->rear - q->front +1)%q->capacity;
}
	

int enqueue(buffer_t *q, unsigned char *src,  int len)
{	
	if(DEBUG)printf("buf_size=%d\n",buf_size(q));
	if(is_full(q))return 0;
	else if(len > (q->capacity-buf_size(q)))return 0;
	else
	{
		int tmp = buf_size(q);
		q->rear = (q->rear + len)%q->capacity;
		memcpy(&(q->array[q->rear-len+1]),src,len);
		if(q->front==-1)
		{
			q->front = q->rear - len+1;
		}
		return buf_size(q)-tmp;
	}
}

int dequeue(buffer_t *q,unsigned char *dst, int len)
{
	if(is_empty(q))return 0;
	else
	{
		memcpy(dst,&(q->array[q->front]),len);
		if(q->front==(q->rear - len+1))
			q->front=q->rear=-1;
		else
			q->front=(q->front + len)%q->capacity;
		return 1;
	}
}

/* mtcp functions */

void mtcp_accept(int socket_fd, struct sockaddr_in *client_addr){
	//srand((unsigned)time(NULL));
	//current_ack = rand() & 0x0fffffff;
	sockfd = socket_fd;
	addr = client_addr;
	pthread_mutex_lock(&info_mutex);
	state = INIT;
	pthread_mutex_unlock(&info_mutex);
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
/*
	//change state to 3-way handshake
	pthread_mutex_lock(&info_mutex);
	state = HS3;													
	pthread_mutex_unlock(&info_mutex);
*/

	//wait until wake signal from recv thread
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig,&app_thread_sig_mutex);
	pthread_mutex_unlock(&app_thread_sig_mutex);


	return;
}
int mtcp_read(int socket_fd, unsigned char *buf, int buf_len){
	if(recvfrom_err==-1 || state==NIL || state==END)return -1;
	if(DEBUG)printf("mtcp_read() no error\n");
	while(is_empty(recvbuf))
	{
		if(DEBUG)printf("reading empty buffer, current_state=%d\n",state);
		if(state==HS4)
		{
			if(DEBUG)printf("mtcp_read return 0\n");
			return 0;
		}
		else
		{
			//Wait for data from recv thread
			//wait for send thread wake signal
			pthread_mutex_lock(&app_thread_sig_mutex);
			pthread_cond_wait(&app_thread_sig, &app_thread_sig_mutex);
			pthread_mutex_unlock(&app_thread_sig_mutex);		
		}
	}
	int len = 0;

	if(!is_empty(recvbuf))
	{
		//copy data from internal buff
		
		if(buf_len<buf_size(recvbuf))
		{
			len = buf_len;
		}
		else
		{
			len = buf_size(recvbuf);
		}

		pthread_mutex_lock(&recvbuf_mutex);
		dequeue(recvbuf,buf,len);
		pthread_mutex_unlock(&recvbuf_mutex);		
	}
	
	if(DEBUG)printf("mtcp_read() return %d\n",len);
	return len;

	
/*
	//wake send thread in case it is waiting
	pthread_mutex_lock(&send_thread_sig);
	pthread_cond_signal(&send_thread_sig);
	pthread_mutex_unlock(&send_thread_sig);
*/

}

void mtcp_close(int socket_fd){
if(DEBUG)printf("mtcp_close()\n");
	pthread_join(send_thread_pid,NULL);
	pthread_join(recv_thread_pid,NULL);
	pthread_mutex_lock(&info_mutex);
	state = END;
	pthread_mutex_unlock(&info_mutex);
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
	if(DEBUG)printf("send_thread created\n");
	unsigned char packet[MAX_BUF_SIZE+4];
//	int len;
//	unsigned int last_ack;
	unsigned int seq, last_seq;					
	state_t current_state;
	unsigned char last_type = -1;
	unsigned char sent_type = -1;
	ssize_t sendto_retv = 0;
	pthread_mutex_lock(&send_thread_sig_mutex);			//sig_mutext place outside the loop since send thread should be waken only if it is waitng. otherwise important wake may be lost
	do
	{
		//Sleep						
		
		pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);
		
		
		//Check state
		pthread_mutex_lock(&info_mutex);
		last_type = last_recv_type;
		current_state = state;
		last_seq = last_ack;
		seq = current_ack;
		pthread_mutex_unlock(&info_mutex);
		if(DEBUG)printf("send thread waken, current_state=%d, last_type=%d\n",current_state,last_type);
		if(current_state==HS3 && last_type==SYN)
		{
			//Send SYNACK
			if(DEBUG)printf("send SYNACK\n");
			sent_type = SYNACK;
			seq++;
			create_packet(packet,SYNACK,seq,NULL,0);
			sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)client,addrlen);
			if(DEBUG)printf("sento_retv=%d,errno=%d\n",sendto_retv,errno);
			//sleep
/*
			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);
			pthread_mutex_unlock(&send_thread_sig_mutex);
*/
			/*
			else if(last_type==ACK)
			{				
				//Wake app thread cause mtcp_accept() return
				pthread_mutex_lock(&app_thread_sig_mutex);
				pthread_cond_signal(&app_thread_sig);	
				pthread_mutex_unlock(&app_thread_sig_mutex);	
			}
			*/
		}
		else if(current_state==RW && last_type==DATA)
		{
			//Send ACK
			if(DEBUG)printf("send ACK=%d\n",seq);
			sent_type = ACK;
			create_packet(packet,ACK,seq,NULL,0);
			sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)client,addrlen);

			//wake app thread in case read() is blocking, only if acknowledging new DATA. Reack should not wake
			if(last_seq!=seq)
			{
				pthread_mutex_lock(&app_thread_sig_mutex);
				pthread_cond_signal(&app_thread_sig);	
				pthread_mutex_unlock(&app_thread_sig_mutex);				
			}

		}
		else if(current_state==HS4 && last_type==FIN)
		{
			if(DEBUG)printf("send FINACK\n");
			sent_type = FINACK;
			seq++;
			create_packet(packet,FINACK,seq,NULL,0);
			sendto_retv = sendto(sockfd,(void *)packet,4,0,(struct sockaddr *)client,addrlen);
		}
		pthread_mutex_lock(&info_mutex);
		last_sent_type = sent_type;
		pthread_mutex_unlock(&info_mutex);

	}while(sent_type!=FINACK);
	pthread_mutex_unlock(&send_thread_sig_mutex);
	if(DEBUG)printf("send thread end\n");
	pthread_exit(NULL);
}

static void *receive_thread(){
	if(DEBUG)printf("receive_thread created\n");
	unsigned char packet[MAX_BUF_SIZE+4];
//	unsigned char data[MAX_BUF_SIZE];
	ssize_t len;
	unsigned char last_type = -1;
	unsigned char current_type = -1;
	unsigned int seq;
	state_t current_state;
	client = malloc(sizeof(struct sockaddr_in));
	addrlen = sizeof(struct sockaddr_in);
	do
	{
		if(DEBUG)printf("Monitoring socket\n");
		//Monitor Socket
		len = recvfrom(sockfd,(void *)packet,MAX_BUF_SIZE+4,0,(struct sockaddr *)client,&addrlen);
		current_type = get_packet_type(packet);
		seq = get_packet_seq(packet);
		if(DEBUG)printf("received type: %d, seq=%d\n", current_type,seq);
		//Check & update state
		pthread_mutex_lock(&info_mutex);
		current_state = state;
		last_recv_type = current_type;
		last_type = last_sent_type;
		last_ack = current_ack;
		current_ack = seq + len - 4;
		recvfrom_err = len;
		pthread_mutex_unlock(&info_mutex);
		
		if(DEBUG)printf("current_state=%d,last_type=%d\n",current_state,last_type);
		if(current_state==INIT && current_type==SYN)								//initiated 3-way handshake
		{
			if(DEBUG)printf("start HS3\n");
			memcpy(addr,client,addrlen);
			if(DEBUG)printf("sin_family=%d,sin_port=%d,in_addr=%d,addrlen=%d\n",client->sin_family,client->sin_port,client->sin_addr.s_addr,addrlen);
			pthread_mutex_lock(&info_mutex);
			state = HS3;
			pthread_mutex_unlock(&info_mutex);

			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_signal(&send_thread_sig);
			pthread_mutex_unlock(&send_thread_sig_mutex);
		}		
		else if(current_state==HS3 && last_type==SYNACK && current_type==ACK)		//finish 3-way handshake
		{
			if(DEBUG)printf("finish HS3\n");
			//change state to read/write
			pthread_mutex_lock(&info_mutex);
			state = RW;													
			pthread_mutex_unlock(&info_mutex);			
			//wake app thread cause mtcp_accept() to return
			pthread_mutex_lock(&app_thread_sig_mutex);
			pthread_cond_signal(&app_thread_sig);
			pthread_mutex_unlock(&app_thread_sig_mutex);	
		}
		else if(current_state==RW && (last_type==ACK || last_type==SYNACK) && current_type==DATA)		//Read data
		{
			if(DEBUG)printf("received RW data with len=%d\n",len);
			if(current_ack != last_ack)		//write data to buffer only if the recv data is new (in case of ACK lost, data will be duplicate)
			{
				int retv=enqueue(recvbuf,&packet[4],len-4);
				if(DEBUG)printf("enqueue retv=%d\n",retv);
			}
			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_signal(&send_thread_sig);
			pthread_mutex_unlock(&send_thread_sig_mutex);	


		}
		else if(current_state==RW && current_type==FIN)							//initiated 4-way handshake
		{
			if(DEBUG)printf("start HS4\n");
			pthread_mutex_lock(&info_mutex);
			state = HS4;
			pthread_mutex_unlock(&info_mutex);

			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_signal(&send_thread_sig);
			pthread_mutex_unlock(&send_thread_sig_mutex);
		}

	}while(!(last_type==FINACK && current_type==ACK));

	//wake app thread in case read() blocking
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_signal(&app_thread_sig);
	pthread_mutex_unlock(&app_thread_sig_mutex);
	free(client);
	if(DEBUG)printf("recv thread end\n");
	pthread_exit(NULL);
}
