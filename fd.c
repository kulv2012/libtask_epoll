#include "taskimpl.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>


enum
{
	MAXFD = 1024
};

static struct epoll_event epoll_recv_events[MAXFD] ;
static int g_epollfd ;
static int startedfdtask;
static Tasklist sleeping;
static int sleepingcounted;
static uvlong nsec(void);

void prepare_fdtask(){

	g_epollfd = epoll_create(2);//Since Linux 2.6.8, the size argument is ignored, but must be greater 0 
	if(g_epollfd < 0){
		printf("epoll_create failed. errno:%d, errmsg:%s.\n", errno, strerror(errno));
		exit(errno);
	}
	taskcreate(fdtask, 0, 32768);//这个是IO等待poll的线程，所有阻塞IO都走这里进行监听，唤醒等
}

void
fdtask(void *v)
{
	int i, ms;
	Task *t;
	uvlong now;
	
	tasksystem();//把自己设置为系统级协程，不会taskexit退出
	taskname("fdtask");
	for(;;){
		/* let everyone else run */
		while(taskyield() > 0)
			;
		/* we're the only one runnable - poll for i/o */
		errno = 0;
		taskstate("poll");
		if((t=sleeping.head) == nil){
			ms = -1;//没有人在sleep，所以就poll一直等待了，这个好危险啊，
			//如果上层不小心yeild了，并且没有dalay的，然后所有fd都没有活跃。那就完蛋了
		}else{
			/* sleep at most 5s */
			now = nsec();
			if(now >= t->alarmtime)
				ms = 0;
			else if(now+5*1000*1000*1000LL >= t->alarmtime)
				ms = (t->alarmtime - now)/1000000;
			else
				ms = 5000;
		}
		int retval = epoll_wait( g_epollfd, epoll_recv_events, MAXFD, ms) ;
		if( retval >= 0){
			for( i=0; i < retval; i++){
				taskready( epoll_recv_events[i].data.ptr) ;//变为可执行状态
			}
		} else if( retval == EINTR){
			continue ;
		} else if (retval < 0){
			fprint(2, "epoll: %s\n", strerror(errno));
			taskexitall(0);
		}

		now = nsec();
		while((t=sleeping.head) && now >= t->alarmtime){
			deltask(&sleeping, t);//看看定时器有没有到时间的 
			if(!t->system && --sleepingcounted == 0)
				taskcount--;
			taskready(t);
		}
	}
}

uint
taskdelay(uint ms)
{//协程的等待函数，这里根据这个协程的等待时间，在sleeping列表里面找一个合适的位置挂到链表里面
	//然后触发线程切换
	uvlong when, now;
	Task *t;
	
	if(!startedfdtask){
		startedfdtask = 1;
		prepare_fdtask();
	}

	now = nsec();
	when = now+(uvlong)ms*1000000;
	for(t=sleeping.head; t!=nil && t->alarmtime < when; t=t->next)
		;

	if(t){
		taskrunning->prev = t->prev;
		taskrunning->next = t;
	}else{
		taskrunning->prev = sleeping.tail;
		taskrunning->next = nil;
	}
	
	t = taskrunning;
	t->alarmtime = when;
	if(t->prev)
		t->prev->next = t;
	else
		sleeping.head = t;
	if(t->next)
		t->next->prev = t;
	else
		sleeping.tail = t;

	if(!t->system && sleepingcounted++ == 0)
		taskcount++;
	taskswitch();

	return (nsec() - now)/1000000;
}

void
fdwait(int *fd, int rw)
{//按需启动fdtask这个异步I/O控制协程，将当前FD加入到poll数组中。进行协程切换。
	int addedmask = 0;
	int oldmask = 0;
	struct epoll_event ee;

	if(!startedfdtask){
		startedfdtask = 1;
		prepare_fdtask();
	}
	taskstate("fdwait for %s", rw=='r' ? "read" : rw=='w' ? "write" : "error");

	oldmask |= (0x80000000&*fd) != 0 ? EPOLLIN : 0 ;
	oldmask |= (0x40000000&*fd) != 0 ? EPOLLOUT : 0 ;
	addedmask = 0;
	switch(rw){
	case 'r':
		addedmask = EPOLLIN ;
		break;
	case 'w':
		addedmask = EPOLLOUT ;
		break;
	}

	ee.data.u64 = 0; /* avoid valgrind warning */
	//将这个FD挂入到epoll里面，这里面是由fdtask协程进行等待唤醒等管理的、
	//等这个FD有事件的时候，会将本协程设置为可运行的状态，并且fdtask也会主动yeild让出CPU。
	if( (addedmask | oldmask) != oldmask ){//add it if need
		ee.events = oldmask|addedmask ;
		ee.data.ptr = taskrunning;
		int op = oldmask == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
		if (epoll_ctl(g_epollfd, op, 0x3FFFFFFF&*fd , &ee) == -1){
			printf("epoll_ctl pre failed. errno:%d, errmsg:%s, state(%s)\n", errno, strerror(errno), taskgetstate());
			exit(errno);
		}
	}
	taskswitch();//注意这里并没有修改这个协程的运行状态，这样他下次还可能跑起来

	if( (addedmask | oldmask) != oldmask  ){ //说明刚才我增加过，那么这里需要从当前状态中，去掉刚刚加入的。 这里如果另外的协程加入了新的事件，就会出现.
		//最好是代码确认读取完成后，显示删除
		oldmask |= (0x80000000&*fd) != 0 ? EPOLLIN : 0 ;
		oldmask |= (0x40000000&*fd) != 0 ? EPOLLOUT : 0 ;
		ee.events = oldmask & (~ addedmask ) ;
		//int op = oldmask == addedmask  ? EPOLL_CTL_DEL : EPOLL_CTL_MOD ;
		int op = ee.events == 0 ? EPOLL_CTL_DEL : EPOLL_CTL_MOD ;
		if (epoll_ctl(g_epollfd, op, 0x3FFFFFFF&*fd, &ee) == -1){
			printf("epoll_ctl post failed. errno:%d, errmsg:%s, state(%s)\n", errno, strerror(errno), taskgetstate());
			exit(errno);
		}
		if( addedmask == EPOLLIN) *fd = 0x7FFFFFFF&*fd ;
		if( addedmask == EPOLLOUT) *fd = 0xBFFFFFFF&*fd ;
	}
	/*
	 不过这里多唤醒一次，当前协程也就是再次尝试I/O，基本还是会EAGAIN， 然后又调用fdwait，又睡下去。这样不会有bug，但会浪费CPU？

PS: 后来想了想，这个会的，因为当前协程在taskscheduler里面调度它运行的时候，使用了deltask(&taskrunqueue, t);//从待调度链表中移出来，调度它运行，因此现在我要再fdwait里面直接调用taskswitch();，那么当前这个协程是不会被加到taskrunqueue链表里面，也就没有机会得到执行。

那么什么时候得到执行呢？答案是：只有当有人主动将其加到taskrunqueue里面，才能执行，这个人就是fdtask I/O监听协程，这是唯一的机会。所以写代码的时候，如果要切换协程，一定得想清楚这一点，别知道怎么切换出去了，不知道什么时候该切换回来就悲剧了。
	 */
}

/* Like fdread but always calls fdwait before reading. */
int
fdread1(int* pfd, void *buf, int n)
{
	int m;
	
	do
		fdwait(pfd, 'r');
	while((m = read( 0x3FFFFFFF&*pfd , buf, n)) < 0 && errno == EAGAIN);
	return m;
}

int
fdread(int* pfd, void *buf, int n)
{
	int m;
	
	while((m=read( 0x3FFFFFFF&*pfd , buf, n)) < 0 && errno == EAGAIN)
		fdwait(pfd, 'r');
	return m;
}

int
fdwrite(int* pfd, void *buf, int n)
{
	int m, tot;
	
	for(tot=0; tot<n; tot+=m){
		while((m=write( 0x3FFFFFFF&*pfd , (char*)buf+tot, n-tot)) < 0 && errno == EAGAIN)
			fdwait(pfd, 'w');//关键：如果写入时返回EAGAIN说明差不多了，得过会才能写入。那么这里需要放入epoll，把本协程挂起
		if(m < 0)
			return m;
		if(m == 0)
			break;
	}
	return tot;
}

int
fdnoblock(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL)|O_NONBLOCK);
}

static uvlong
nsec(void)
{
	struct timeval tv;

	if(gettimeofday(&tv, 0) < 0)
		return -1;
	return (uvlong)tv.tv_sec*1000*1000*1000 + tv.tv_usec*1000;
}

