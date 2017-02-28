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

typedef enum {INIT,HS3,RW,HS4} state_t;			

/* -------------------- Global Variables -------------------- */
int sockfd;
struct sockaddr_in *addr;
state_t state;
unsigned int current_ack;

/* ThreadID for Sending Thread and Receiving Thread */
static pthread_t send_thread_pid;
static pthread_t recv_thread_pid;

static pthread_cond_t app_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t app_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t send_thread_sig = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t send_thread_sig_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t info_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Connect Function Call (mtcp Version) */
void mtcp_connect(int socket_fd, struct sockaddr_in *server_addr){
	srand((unsigned)time(NULL));
	sockfd = socket_fd;
	addr = server_addr;
	state = INIT;
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
	
	while(state != HS3)									//wake send thread until handshake starts
	{
		pthread_mutex_lock(&send_thread_sig_mutex);
		pthread_cond_signal(&send_thread_sig,&send_thread_sig_mutex);
		pthread_mutex_unlock(&send_thread_sig_mutex);		
	}
	
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig,&app_thread_sig_mutex);	//wait for send thread
	pthread_mutex_unlock(&app_thread_sig_mutex);	
	
	return;
}

/* Write Function Call (mtcp Version) */
int mtcp_write(int socket_fd, unsigned char *buf, int buf_len){



}

/* Close Function Call (mtcp Version) */
void mtcp_close(int socket_fd){
	//change state to 4-way handshake
	pthread_mutex_lock(&state_mutex);
	state = HS4;						
	pthread_mutex_unlock(&state_mutex);
	//wake send thread
	pthread_mutex_lock(&send_thread_sig_mutex);
	pthread_cond_signal(&send_thread_sig,&send_thread_sig_mutex);
	pthread_mutex_unlock(&send_thread_sig_mutex);	
	//waite for send thread finish 4-way handshake
	pthread_mutex_lock(&app_thread_sig_mutex);
	pthread_cond_wait(&app_thread_sig,&app_thread_sig_mutex);	//wait for send thread
	pthread_mutex_unlock(&app_thread_sig_mutex);	
	
	pthread_join(send_thread_pid,NULL);
	pthread_join(recv_thread_pid,NULL);
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
	pthread_mutex_lock(&send_thread_sig_mutex);
	pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);	//wait for app thread
	pthread_mutex_unlock(&send_thread_sig_mutex);
	
	pthread_mutex_lock(&state_mutex);
	state = HS3;						//tell app thread that send thread is woken
	pthread_mutex_unlock(&state_mutex);
	
	//3-way handshake starts 
	unsigned int seq = rand();					//initialize sequence number
	struct timespect abstime;
	//send a SYN
	create_packet(packet,SYN,seq,NULL,0);
	sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));
	
	clock_gettime(CLOCK_REALTIME,&abstime);
	abstime.tv_sec++;							//generate timespec for timedwait
	
	pthread_mutex_lock(&send_thread_sig_mutex);
	while(pthread_cond_timedwait(&send_thread_sig,&send_thread_sig_mutex)==ETIMEDOUT)	//wait for recv thread
	{
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
	
	//Go into data transfer state
	pthread_mutex_lock(&state_mutex);
	state = RW;						
	pthread_mutex_unlock(&state_mutex);
	//wait for app thread write/close
	while(state!=HS4)				//break loop if 4-way handshake initiated					
	{
		if()			//have data to send
		{
			
		}
		else			//buffer empty
		{
			pthread_mutex_lock(&send_thread_sig_mutex);
			pthread_cond_wait(&send_thread_sig,&send_thread_sig_mutex);	//wait for app thread
			pthread_mutex_unlock(&send_thread_sig_mutex);				
		}
	}
	//send FIN
	seq = current_ack;
	create_packet(packet,FIN,seq,NULL,0);
	sento(sockfd,(void *)packet,4,NULL,(struct sockaddr *)addr,sizeof(addr));	
	clock_gettime(CLOCK_REALTIME,&abstime);
	abstime.tv_sec++;														//generate timespec for timedwait	
	pthread_mutex_lock(&send_thread_sig_mutex);
	while(pthread_cond_timedwait(&send_thread_sig,&send_thread_sig_mutex)==ETIMEDOUT)	//wait for recv thread
	{
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
	//Monitor for SYNACK
	unsigned char packet[MAX_BUF_SIZE+4];
	size_t len;
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
	
	//Monitor for ACK and FINACK
	do
	{
		len = recvfrom(sockfd,(void *)packet,MAX_BUF_SIZE+4,NULL,(struct sockaddr *)addr,sizeof(addr));
		if(get_packet_type(packet)==ACK)
		{
			
		}
	}while(get_packet_type(packet)!=FINACK);
	//FINACK received
	pthread_mutex_lock(&info_mutex);
	current_ack = get_packet_ack(packet);					//get ack number from recv packet
	pthread_mutex_unlock(&info_mutex);
	
	pthread_mutex_lock(&send_thread_sig_mutex);
	pthread_cond_signal(&send_thread_sig,&app_thread_sig_mutex);		//wake send thread
	pthread_mutex_unlock(&send_thread_sig_mutex);	
	
	pthread_exit(NULL);
}













