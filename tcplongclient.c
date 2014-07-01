#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <task.h>
#include <stdlib.h>
#include <sys/socket.h>

enum
{
	STACK = 32768
};

char *server;
int port;
void proxytask(void*);
void rwtask(void*);

void
taskmain(int argc, char **argv)
{
	int count ;
	int i = 0 ;
	
	if(argc != 4){
		fprintf(stderr, "usage: tcplongclient count server remoteport\n");
		taskexitall(1);
	}
	count = atoi(argv[1]);
	server = argv[2];
	port = atoi(argv[3]);

	for(i = 0 ; i < count ; ++i){
		taskcreate(proxytask, (void*)i, STACK);
	}
}

void
proxytask(void *v)
{
	int fd;
	int n , tmp; 
	char buf[256] ;

	tmp = (int)v;
	if((fd = netdial(TCP, server, port)) < 0){
		close(fd);
		return;
	}
	
	fprintf(stderr, "connected to %s:%d\n", server, port);
	while(1){
		sprintf(buf, "%d", ++tmp) ;
		fdwrite(&fd, buf, sizeof(buf));
		n = fdread(&fd, buf, sizeof buf) ;
		if( n <= 0 )  break ;
		buf[n] = 0 ;
		printf("recv: %s\n", buf) ;

		taskdelay(1000) ;
	}

}


