#include "taskimpl.h"
#include <sys/poll.h>
#include <fcntl.h>

enum
{
	MAXFD = 1024
};

static struct pollfd pollfd[MAXFD];//局部静态的poll数组
static Task *polltask[MAXFD];
static int npollfd;
static int startedfdtask;
static Tasklist sleeping;
static int sleepingcounted;
static uvlong nsec(void);

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
		if(poll(pollfd, npollfd, ms) < 0){
			if(errno == EINTR)
				continue;
			fprint(2, "poll: %s\n", strerror(errno));
			taskexitall(0);
		}

		/* wake up the guys who deserve it */
		for(i=0; i<npollfd; i++){
			while(i < npollfd && pollfd[i].revents){
				taskready(polltask[i]);//将这些有情况的协程设置为可运行状态，这样这个for下一轮的时候就会调用taskyield主动让出CPU
				--npollfd;
				pollfd[i] = pollfd[npollfd];
				polltask[i] = polltask[npollfd];
			}
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
		taskcreate(fdtask, 0, 32768);
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
fdwait(int fd, int rw)
{//按需启动fdtask这个异步I/O控制协程，将当前FD加入到poll数组中。进行协程切换。
	int bits;

	if(!startedfdtask){
		startedfdtask = 1;
		taskcreate(fdtask, 0, 32768);//这个是IO等待poll的线程，所有阻塞IO都走这里进行监听，唤醒等
	}

	if(npollfd >= MAXFD){
		fprint(2, "too many poll file descriptors\n");
		abort();
	}
	
	taskstate("fdwait for %s", rw=='r' ? "read" : rw=='w' ? "write" : "error");
	bits = 0;
	switch(rw){
	case 'r':
		bits |= POLLIN;
		break;
	case 'w':
		bits |= POLLOUT;
		break;
	}

	//将这个FD挂入到pollfd里面，这里面是由fdtask协程进行等待唤醒等管理的、
	//等这个FD有事件的时候，会将本协程设置为可运行的状态，并且fdtask也会主动yeild让出CPU。
	polltask[npollfd] = taskrunning;
	pollfd[npollfd].fd = fd;
	pollfd[npollfd].events = bits;
	pollfd[npollfd].revents = 0;
	npollfd++;
	taskswitch();//注意这里并没有修改这个协程的运行状态，这样他下次还可能跑起来
	/*
	 不过这里多唤醒一次，当前协程也就是再次尝试I/O，基本还是会EAGAIN， 然后又调用fdwait，又睡下去。这样不会有bug，但会浪费CPU？

PS: 后来想了想，这个会的，因为当前协程在taskscheduler里面调度它运行的时候，使用了deltask(&taskrunqueue, t);//从待调度链表中移出来，调度它运行，因此现在我要再fdwait里面直接调用taskswitch();，那么当前这个协程是不会被加到taskrunqueue链表里面，也就没有机会得到执行。

那么什么时候得到执行呢？答案是：只有当有人主动将其加到taskrunqueue里面，才能执行，这个人就是fdtask I/O监听协程，这是唯一的机会。所以写代码的时候，如果要切换协程，一定得想清楚这一点，别知道怎么切换出去了，不知道什么时候该切换回来就悲剧了。
	 */
}

/* Like fdread but always calls fdwait before reading. */
int
fdread1(int fd, void *buf, int n)
{
	int m;
	
	do
		fdwait(fd, 'r');
	while((m = read(fd, buf, n)) < 0 && errno == EAGAIN);
	return m;
}

int
fdread(int fd, void *buf, int n)
{
	int m;
	
	while((m=read(fd, buf, n)) < 0 && errno == EAGAIN)
		fdwait(fd, 'r');
	return m;
}

int
fdwrite(int fd, void *buf, int n)
{
	int m, tot;
	
	for(tot=0; tot<n; tot+=m){
		while((m=write(fd, (char*)buf+tot, n-tot)) < 0 && errno == EAGAIN)
			fdwait(fd, 'w');//关键：如果写入时返回EAGAIN说明差不多了，得过会才能写入。那么这里需要放入epoll，把本协程挂起
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

