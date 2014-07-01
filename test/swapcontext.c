#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>

static ucontext_t uctx_main, uctx_func1, uctx_func2;

#define handle_error(msg) \
	do { perror(msg); exit(EXIT_FAILURE); } while (0)

	static void
func1(void)
{
	printf("func1: started\n");
	if (swapcontext(&uctx_func1, &uctx_func2) == -1)
		handle_error("swapcontext");
	printf("func1: returning\n");
}

	static void
func2(void)
{
	printf("func2: started\n");
	if (swapcontext(&uctx_func2, &uctx_func1) == -1)
		handle_error("swapcontext");
	printf("func2: returning\n");
}

	int
main(int argc, char *argv[])
{
	char func1_stack[16384];
	char func2_stack[16384];

	if (getcontext(&uctx_func1) == -1)
		handle_error("getcontext");
	uctx_func1.uc_stack.ss_sp = func1_stack;//2的运行地址设置为这个
	uctx_func1.uc_stack.ss_size = sizeof(func1_stack);
	uctx_func1.uc_link = &uctx_main;//设置我结束的时候，运行的是哪个上下文
	makecontext(&uctx_func1, func1, 0);//make里面会设置堆栈参数内容，设置堆栈指针和指令指针等

	if (getcontext(&uctx_func2) == -1)
		handle_error("getcontext");
	uctx_func2.uc_stack.ss_sp = func2_stack;
	uctx_func2.uc_stack.ss_size = sizeof(func2_stack);
	/* Successor context is f1(), unless argc > 1 */
	uctx_func2.uc_link = (argc > 1) ? NULL : &uctx_func1;//2结束的时候，运行的是1
	makecontext(&uctx_func2, func2, 0);

	if (swapcontext(&uctx_main, &uctx_func2) == -1)//首先将main挂起，运行2，
		handle_error("swapcontext");

	printf("main: exiting\n");
}
