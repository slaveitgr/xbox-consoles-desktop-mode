// SPDX-License-Identifier: GPL-2.0-only
// Self-authored static Linux x86-64 guest; execute ONLY through CPU64.
// Check real process/thread IDs, fork relationships, liveness and CLONE_THREAD.
typedef unsigned long long U64;
static volatile long owner_pid,owner_tid,thread_pid,thread_tid;
static volatile int thread_done,thread_result;
static volatile U64 private_word=0x1122334455667788ULL;
char thread_stack[65536] __attribute__((aligned(16)));
static long call6(long n,long a,long b,long c,long d,long e,long f){register long r10 __asm__("r10")=d;register long r8 __asm__("r8")=e;register long r9 __asm__("r9")=f;long out;__asm__ volatile("syscall":"=a"(out):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");return out;}
#define SAY(s) call6(1,1,(long)(s),sizeof(s)-1,0,0,0)
#define CHECK(c,s,n) do{if(!(c)){SAY("PASSFAIL_ID FAIL: " s "\n");return n;}SAY("PASSFAIL_ID PASS: " s "\n");}while(0)
int thread_main(void){
 thread_pid=call6(39,0,0,0,0,0,0);thread_tid=call6(186,0,0,0,0,0,0);
 if(thread_pid!=owner_pid)thread_result=31;
 else if(thread_tid<=0||thread_tid==owner_tid)thread_result=32;
 else if(call6(62,thread_pid,0,0,0,0,0)!=0)thread_result=33;
 thread_done=1;return thread_result;
}
long clone_thread(void);
__asm__(".text\n.global clone_thread\nclone_thread:\n"
        "mov $0x10f00,%edi\nlea thread_stack+65536(%rip),%rsi\n"
        "xor %edx,%edx\nxor %r10d,%r10d\nxor %r8d,%r8d\n"
        "mov $56,%eax\nsyscall\ntest %rax,%rax\njnz 1f\n"
        "call thread_main\nmov %eax,%edi\nmov $60,%eax\nsyscall\nud2\n1: ret\n");
static int child_test(long parent){
 CHECK(call6(110,0,0,0,0,0,0)==parent,"child getppid matches parent's process ID",20);
 long childpid=call6(39,0,0,0,0,0,0),childtid=call6(186,0,0,0,0,0,0);
 CHECK(childpid>0&&childpid!=parent&&childtid>0,"fork creates new process and thread identities",21);
 CHECK(call6(62,childpid,0,0,0,0,0)==0,"fork child process ID is recognized by kill(pid,0)",22);
 private_word=0x8877665544332211ULL;return 0;
}
static int test(void){
 owner_pid=call6(39,0,0,0,0,0,0);owner_tid=call6(186,0,0,0,0,0,0);
 CHECK(owner_pid>0&&owner_tid>0,"getpid and gettid return positive identities",9);
 CHECK(call6(62,owner_pid,0,0,0,0,0)==0,"getpid returns a process recognized by kill(pid,0)",10);
 long child=call6(56,17,0,0,0,0,0);CHECK(child>=0,"actual fork succeeds",11);
 if(!child){call6(60,child_test(owner_pid),0,0,0,0,0);__builtin_unreachable();}
 int status=0;long result;do{result=call6(61,child,(long)&status,0,0,0,0);}while(result==-4);
 CHECK(result==child&&status==0,"parent reaps passing identity child",12);
 CHECK(private_word==0x1122334455667788ULL,"fork preserves parent private memory",13);
 CHECK(call6(39,0,0,0,0,0,0)==owner_pid,"parent process identity remains stable after fork",14);
 long tid=clone_thread();CHECK(tid>0,"actual CLONE_THREAD succeeds",15);
 for(int i=0;i<1000000&&!thread_done;i++)call6(24,0,0,0,0,0,0);
 CHECK(thread_done&&thread_result==0,"thread shares getpid but has its own gettid and live process identity",16);
 CHECK(thread_pid==owner_pid&&thread_tid==tid,"clone result equals actual child thread ID",17);
 CHECK(call6(39,0,0,0,0,0,0)==owner_pid&&call6(186,0,0,0,0,0,0)==owner_tid,"main IDs remain stable after thread creation",18);
 return 0;
}
void _start(void){call6(60,test(),0,0,0,0,0);__builtin_unreachable();}
