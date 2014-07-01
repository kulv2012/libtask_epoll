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

void proxytask(void*);
void rwtask(void*);


void
taskmain(int argc, char **argv)
{
	int cfd, fd;
	int rport;
	char remote[16];
	
	if(argc != 2){
		fprintf(stderr, "usage: tcplongconnection localport \n");
		taskexitall(1);
	}

	if((fd = netannounce(TCP, 0, atoi(argv[1]))) < 0){
		fprintf(stderr, "cannot announce on tcp port %d: %s\n", atoi(argv[1]), strerror(errno));
		taskexitall(1);
	}
	fdnoblock(fd);
	while((cfd = netaccept(&fd, remote, &rport)) >= 0){
		fprintf(stderr, "connection from %s:%d\n", remote, rport);
		taskcreate(proxytask, (void*)cfd, STACK);
	}
}

void
proxytask(void *v)
{
	int fd ;
	int n = 0 ;
	char buf[1024];
	fd = (int)v;
	
	while((n = fdread(&fd, buf, sizeof buf)) > 0) {
		fdwrite(&fd, buf, n);
	}
	close(fd);

}


