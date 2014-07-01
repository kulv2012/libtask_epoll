/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"
#include <fcntl.h>
#include <stdio.h>

int	taskdebuglevel;
int	taskcount;//当前OK的协程数目，不包括系统级别的协程
int	tasknswitch;//总协程切换次数
int	taskexitval;
Task	*taskrunning;//当前正在运行的协程指针

Context	taskschedcontext;//这个就是主协程的上下文，进行切换的时候会换起这个上下文，也就是taskscheduler函数
Tasklist	taskrunqueue;//所有可运行的协程的双向链表，不包括当前正在运行的

Task	**alltask;//协程数组
int		nalltask;//alltask数组中下一个空槽位，当为64的倍数时需要扩容

static char *argv0;
static	void		contextswitch(Context *from, Context *to);

static void
taskdebug(char *fmt, ...)
{
	va_list arg;
	char buf[128];
	Task *t;
	char *p;
	static int fd = -1;

return;
	va_start(arg, fmt);
	vfprint(1, fmt, arg);
	va_end(arg);
return;

	if(fd < 0){
		p = strrchr(argv0, '/');
		if(p)
			p++;
		else
			p = argv0;
		snprint(buf, sizeof buf, "/tmp/%s.tlog", p);
		if((fd = open(buf, O_CREAT|O_WRONLY, 0666)) < 0)
			fd = open("/dev/null", O_WRONLY);
	}

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	t = taskrunning;
	if(t)
		fprint(fd, "%d.%d: %s\n", getpid(), t->id, buf);
	else
		fprint(fd, "%d._: %s\n", getpid(), buf);
}

static void
taskstart(uint y, uint x)
{
	Task *t;
	ulong z;

	//将2个四字节组成一个8字节64位的地址，指向了所属的Task结构
	z = x<<16;	/* hide undefined 32-bit shift from 32-bit compilers */
	z <<= 16;
	z |= y;
	t = (Task*)z;

//print("taskstart %p\n", t);
	t->startfn(t->startarg);//调用用户的启动地址
//print("taskexits %p\n", t);
	taskexit(0);
//print("not reacehd\n");
}

static int taskidgen;

static Task*
taskalloc(void (*fn)(void*), void *arg, uint stack)
{//申请Task结构，初始化信号，堆栈，运行函数等，调用makecontext模拟堆栈内容，设置堆栈指针esp位置灯
	Task *t;
	sigset_t zero;
	uint x, y;
	ulong z;

	/* allocate the task and stack together */
	t = malloc(sizeof *t+stack);//存放Task结构和后面的堆栈
	if(t == nil){
		fprint(2, "taskalloc malloc: %r\n");
		abort();
	}
	memset(t, 0, sizeof *t);
	t->stk = (uchar*)(t+1);//注意这里加1，实际上加的是一个Task结构，也就是移动了sizeof(Task).
	t->stksize = stack;
	t->id = ++taskidgen;//就是个全局静态变量自增的id
	t->startfn = fn;
	t->startarg = arg;

	/* do a reasonable initialization */
	memset(&t->context.uc, 0, sizeof t->context.uc);
	sigemptyset(&zero);
	sigprocmask(SIG_BLOCK, &zero, &t->context.uc.uc_sigmask);//这里获取一下现在的信号屏蔽字到uc_sigmask存起来，切换协程的时候会设置的

	/* must initialize with current context */
	if(getcontext(&t->context.uc) < 0){//初始化获取一下当前上下文状态，下面在修改堆栈，指令等地址
		fprint(2, "getcontext: %r\n");
		abort();
	}

	/* call makecontext to do the real work. */
	/* leave a few words open on both ends */
	t->context.uc.uc_stack.ss_sp = t->stk+8;//堆栈起始位置，使用的时候会从结束位置开始向下使用的
	t->context.uc.uc_stack.ss_size = t->stksize-64;
#if defined(__sun__) && !defined(__MAKECONTEXT_V2_SOURCE)		/* sigh */
#warning "doing sun thing"
	/* can avoid this with __MAKECONTEXT_V2_SOURCE but only on SunOS 5.9 */
	t->context.uc.uc_stack.ss_sp = 
		(char*)t->context.uc.uc_stack.ss_sp
		+t->context.uc.uc_stack.ss_size;
#endif
	/*
	 * All this magic is because you have to pass makecontext a
	 * function that takes some number of word-sized variables,
	 * and on 64-bit machines pointers are bigger than words.
	 */
//print("make %p\n", t);
	z = (ulong)t;
	y = z;
	z >>= 16;	/* hide undefined 32-bit shift from 32-bit compilers */
	x = z>>16;
	makecontext(&t->context.uc, (void(*)())taskstart, 2, y, x);
	//由于makecontext需要参数必须是4个字节的，所以这里讲t的指针转为64位8个字节，然后分y,x 2个四字节传参数
	//以备makecontext能够模拟设置堆栈传参,并设置sp等堆栈指针位置

	return t;
}

int
taskcreate(void (*fn)(void*), void *arg, uint stack)
{
	int id;
	Task *t;

	t = taskalloc(fn, arg, stack);//初始化申请Task数据结构，设置堆栈内容等
	taskcount++;//当前OK的协程数目，不包括系统级别的协程
	id = t->id;
	if(nalltask%64 == 0){//一次申请64个槽位，只能往后放
		alltask = realloc(alltask, (nalltask+64)*sizeof(alltask[0]));
		if(alltask == nil){
			fprint(2, "out of memory\n");
			abort();
		}
	}
	t->alltaskslot = nalltask;
	alltask[nalltask++] = t;
	taskready(t);//加入taskrunqueue的运行队列
	return id;
}

void
tasksystem(void)
{
	if(!taskrunning->system){
		taskrunning->system = 1;
		--taskcount;
	}
}

void
taskswitch(void)
{//主动进行协程切换，其实就是切换到main协程，到那里去进行实际的切换
	needstack(0);//检测堆栈溢出
	contextswitch(&taskrunning->context, &taskschedcontext);
}

void
taskready(Task *t)
{
	t->ready = 1;//状态设置为可以运行的状态，然后加到运行 任务队列末尾里面去
	addtask(&taskrunqueue, t);
}

int
taskyield(void)
{
	int n;
	
	n = tasknswitch;//用来计算自愿放弃协程后，到恢复所发生的切换次数
	taskready(taskrunning);//挂到taskrunqueue后面
	taskstate("yield");
	taskswitch();
	return tasknswitch - n - 1;
}

int
anyready(void)
{
	return taskrunqueue.head != nil;
}

void
taskexitall(int val)
{
	exit(val);
}

void
taskexit(int val)
{
	taskexitval = val;
	taskrunning->exiting = 1;
	taskswitch();
}

static void
contextswitch(Context *from, Context *to)
{//调用glibc库进行真正的线程切换动作，里面主要就是各个寄存器，堆栈指针esp，指令指针eip的切换等
	if(swapcontext(&from->uc, &to->uc) < 0){
		fprint(2, "swapcontext failed: %r\n");
		assert(0);
	}
}

static void
taskscheduler(void)
{//协程调度函数
	int i;
	Task *t;

	taskdebug("scheduler enter");
	for(;;){
		if(taskcount == 0)
			exit(taskexitval);
		t = taskrunqueue.head;//从头部开始唤起
		if(t == nil){
			fprint(2, "no runnable tasks! %d tasks stalled\n", taskcount);
			exit(1);
		}
		deltask(&taskrunqueue, t);//从待调度链表中移出来，调度它运行
		t->ready = 0;
		taskrunning = t;//标记正在执行的协程
		tasknswitch++;
		taskdebug("run %d (%s)", t->id, t->name);
		contextswitch(&taskschedcontext, &t->context);//真正进行上下文切换，这样就切换到了其他协程运行，比如taskmainstart
//print("back in scheduler\n");
		taskrunning = nil;//把刚刚被切换的协程的指向改为空。
		if(t->exiting){
			if(!t->system)
				taskcount--;
			i = t->alltaskslot;
			alltask[i] = alltask[--nalltask];
			alltask[i]->alltaskslot = i;
			free(t);
		}
	}
}

void**
taskdata(void)
{
	return &taskrunning->udata;
}

/*
 * debugging
 */
void
taskname(char *fmt, ...)
{
	va_list arg;
	Task *t;

	t = taskrunning;
	va_start(arg, fmt);
	vsnprint(t->name, sizeof t->name, fmt, arg);
	va_end(arg);
}

char*
taskgetname(void)
{
	return taskrunning->name;
}

void
taskstate(char *fmt, ...)
{//就设置了一下状态字符串，好废呀
	va_list arg;
	Task *t;

	t = taskrunning;
	va_start(arg, fmt);
	vsnprint(t->state, sizeof t->name, fmt, arg);
	va_end(arg);
}

char*
taskgetstate(void)
{
	return taskrunning->state;
}

void
needstack(int n)
{//简单检测一下堆栈是不是溢出了，因为我们的t->stk指向的是堆栈的底部，所以能够检测的 
	Task *t;

	t = taskrunning;
	
	if((char*)&t <= (char*)t->stk
	|| (char*)&t - (char*)t->stk < 256+n){
		fprint(2, "task stack overflow: &t=%p tstk=%p n=%d\n", &t, t->stk, 256+n);
		abort();
	}
}

static void
taskinfo(int s)
{
	int i;
	Task *t;
	char *extra;

	fprint(2, "task list:\n");
	for(i=0; i<nalltask; i++){
		t = alltask[i];
		if(t == taskrunning)
			extra = " (running)";
		else if(t->ready)
			extra = " (ready)";
		else
			extra = "";
		fprint(2, "%6d%c %-20s %s%s\n", 
			t->id, t->system ? 's' : ' ', 
			t->name, t->state, extra);
	}
}

/*
 * startup
 */

static int taskargc;
static char **taskargv;
int mainstacksize;

static void
taskmainstart(void *v)
{
	taskname("taskmain");
	taskmain(taskargc, taskargv);
}

int
main(int argc, char **argv)
{
	struct sigaction sa, osa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = taskinfo;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGQUIT, &sa, &osa);

#ifdef SIGINFO
	sigaction(SIGINFO, &sa, &osa);
#endif

	argv0 = argv[0];
	taskargc = argc;
	taskargv = argv;

	if(mainstacksize == 0)
		mainstacksize = 256*1024;
	taskcreate(taskmainstart, nil, mainstacksize);//创建初始协程，调用应用层的taskmain函数，此时taskmainstart协程还没有运行，等待调度
	taskscheduler();//进行协程调度
	fprint(2, "taskscheduler returned in main!\n");
	abort();
	return 0;
}

/*
 * hooray for linked lists
 */
void
addtask(Tasklist *l, Task *t)
{//放到链表头部
	if(l->tail){
		l->tail->next = t;
		t->prev = l->tail;
	}else{
		l->head = t;
		t->prev = nil;
	}
	l->tail = t;
	t->next = nil;
}

void
deltask(Tasklist *l, Task *t)
{
	if(t->prev)
		t->prev->next = t->next;
	else
		l->head = t->next;
	if(t->next)
		t->next->prev = t->prev;
	else
		l->tail = t->prev;
}

unsigned int
taskid(void)
{
	return taskrunning->id;
}

