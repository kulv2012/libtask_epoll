#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <task.h>
#include <stdlib.h>

enum{	STACK = 32768 };
char *server;
char *url;
void fetchtask(void*);

void taskmain(int argc, char **argv){
	int i, n;
	
	if(argc != 4){
		fprintf(stderr, "usage: httpload n server url\n");
		taskexitall(1);
	}
	n = atoi(argv[1]);
	server = argv[2];
	url = argv[3];

	for(i=0; i<n; i++){
		taskcreate(fetchtask, 0, STACK);//创建协程，设置为可以运行的状态. 
		//其实fetchtask不是上下文切换的第一个函数，taskstart才是，后者立即调用fetchtask
		while(taskyield() > 1)//主动释放CPU，这里循环其实是为了给其他协程足够的机会
			;
		sleep(1);
	}
}

void fetchtask(void *v) {
	int fd, n;
	char buf[512];
	
	fprintf(stderr, "starting...\n");
	for(;;){
		if((fd = netdial(TCP, server, 80)) < 0){//异步连接服务器，会造成协程切换
			fprintf(stderr, "dial %s: %s (%s)\n", server, strerror(errno), taskgetstate());
			continue;
		}
		snprintf(buf, sizeof buf, "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", url, server);
		fdwrite(fd, buf, strlen(buf));//异步数据读写，这里可能会造成协程切换，因为一定有阻塞操作
		while((n = fdread(fd, buf, sizeof buf)) > 0){///异步读取
			//buf[n] = '\0';
			//printf("buf:%s", buf);
		}
		close(fd);
		write(1, ".", 1);
	}
}

