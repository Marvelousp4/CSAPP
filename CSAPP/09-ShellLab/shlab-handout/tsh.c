/*
 * tsh - A tiny shell program with job control
 *
 * <Put your name and login ID here>
 */
#include <bits/types/sigset_t.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Misc manifest constants */
#define MAXLINE 1024 /* max line size */
#define MAXARGS 128 /* max args on a command line */
#define MAXJOBS 16 /* max jobs at any point in time */
#define MAXJID 1 << 16 /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1 /* running in foreground */
#define BG 2 /* running in background */
#define ST 3 /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char** environ; /* defined in libc extern这个关键字的真正的作用是引用不在同一个文件中的变量或者函数。*/
char prompt[] = "tsh> "; /* command line prompt (DO NOT CHANGE) */
int verbose = 0; /* if true, print additional output */
int nextjid = 1; /* next job ID to allocate */
char sbuf[MAXLINE]; /* for composing sprintf messages */

struct job_t { /* The job struct */
    pid_t pid; /* job PID */
    int jid; /* job ID [1, 2, ...] */
    int state; /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE]; /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */

/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char* cmdline);
int builtin_cmd(char** argv);
void do_bgfg(char** argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char* cmdline, char** argv);
void sigquit_handler(int sig);

void clearjob(struct job_t* job);
void initjobs(struct job_t* jobs);
int maxjid(struct job_t* jobs);
int addjob(struct job_t* jobs, pid_t pid, int state, char* cmdline);
int deletejob(struct job_t* jobs, pid_t pid);
pid_t fgpid(struct job_t* jobs);
struct job_t* getjobpid(struct job_t* jobs, pid_t pid);
struct job_t* getjobjid(struct job_t* jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t* jobs);

void usage(void);
void unix_error(char* msg);
void app_error(char* msg);
typedef void handler_t(int);
handler_t* Signal(int signum, handler_t* handler);

/*
 * main - The shell's main routine
 */
int main(int argc, char** argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h': /* print help message */
            usage();
            break;
        case 'v': /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p': /* don't print a prompt */
            emit_prompt = 0; /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT, sigint_handler); /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler); /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler); /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */
void eval(char* cmdline)
{
    char* argv[MAXARGS];
    char buf[MAXLINE];
    int bg;
    int state;
    pid_t pid;
    sigset_t mask_all, mask_one, prev;

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    state = bg ? BG : FG;

    // 没有参数就退出
    // 其实可以加一个判断参数数量是否正确的语句，比较完整
    if (argv[0] == NULL)
        return;

    // 如果不是内置的命令，则执行
    if (!builtin_cmd(argv)) {
        // 在函数内部加阻塞列表，不然之后可能会出现不痛不痒的bug
        sigfillset(&mask_all); // 将所有信号加入集合
        sigemptyset(&mask_one); // 清空集合
        sigaddset(&mask_one, SIGCHLD); // 将sigchld信号加入集合

        // 如果父进程没有处理子进程结束的信号sigchld，子进程就会变成僵尸进程
        // 所以在fork子进程前阻塞sigchld信号，addjob后解除
        sigprocmask(SIG_BLOCK, &mask_one, &prev);
        if ((pid = fork()) == 0) { // 创建子进程
            // 子进程继承了父进程的阻塞向量，也要解除阻塞，
            // 避免收不到它本身的子进程的信号
            sigprocmask(SIG_SETMASK, &prev, NULL);
            // 改进程组与自己pid一样
            if (setpgid(0, 0) < 0) {
                perror("SETPGID ERROR"); // 输出
                exit(0);
            }
            // 正常运行execve函数会替换内存，不会返回/退出，所以必须要加exit，
            // 否则会一直运行下去，子进程会开始运行父进程的代码
            if (execve(argv[0], argv, environ) < 0) { // 执行
                printf("%s: Command not found\n", argv[0]);
                exit(0);
            }
        }

        if (state == FG) {
            sigprocmask(SIG_BLOCK, &mask_all, NULL); // 添加工作前阻塞所有信号
            addjob(jobs, pid, state, cmdline); // 添加至作业列表
            sigprocmask(SIG_SETMASK, &mask_one, NULL);
            waitfg(pid); // 等待前台进程执行完毕
        } else {
            sigprocmask(SIG_BLOCK, &mask_all, NULL); // 添加工作前阻塞所有信号
            addjob(jobs, pid, state, cmdline); // 添加至作业列表
            sigprocmask(SIG_SETMASK, &mask_one, NULL);
            // 因为这里打印了全局变量，而printf函数是线程不安全的，比如可能会出现读内存的同时另一个线程修改它的情况
            printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); // 打印后台进程信息
        }
        sigprocmask(SIG_SETMASK, &prev, NULL); // 解除阻塞
    }
    return;
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */
int parseline(const char* cmdline, char** argv)
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char* buf = array; /* ptr that traverses command line */
    char* delim; /* points to first space delimiter */
    int argc; /* number of args */
    int bg; /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf) - 1] = ' '; /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') { // 如果是以单引号开头的
        buf++;
        delim = strchr(buf, '\''); // 找到下一个单引号出现的位置
    } else {
        delim = strchr(buf, ' '); // 找到空格出现的位置
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        } else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;

    if (argc == 0) /* ignore blank line */
        return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc - 1] == '&')) != 0) {
        argv[--argc] = NULL;
    }
    return bg;
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.
 */
int builtin_cmd(char** argv)
{
    if (!strcmp(argv[0], "quit"))
        exit(0);
    if (!strcmp(argv[0], "bg") || !strcmp(argv[0], "fg")) {
        do_bgfg(argv);
        return 1;
    }
    if (!strcmp(argv[0], "jobs")) {
        listjobs(jobs);
        return 1;
    }
    if (!strcmp(argv[0], "&"))
        return 1;
    return 0; /* not a builtin command */
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 * SIGCONT 让一个停止(stopped)的进程继续执行. 本信号不能被阻塞.
 * 可以用一个handler来让程序在由stopped状态变为继续执行时完成特定的工作. 例如, 重新显示提示符
 */
void do_bgfg(char** argv)
{
    struct job_t* job = NULL; // 要处理的job
    int state; // 输入的命令
    int id; // 存储jid或pid
    if (!strcmp(argv[0], "bg"))
        state = BG;
    else
        state = FG;
    if (argv[1] == NULL) {
        printf("%s command requires pid or %%jobid argument\n", argv[0]);
        return;
    }
    if (argv[1][0] == '%') {
    }
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 * 这个函数从要求实现阻塞父进程，直到当前的前台进程不再是前台进程了。这里显然要显示地等待信号
 */
void waitfg(pid_t pid)
{
    sigset_t mask;
    sigemptyset(&mask);
    while (!fgpid(jobs)) {
        /*
        该函数相当于
        sigprocmask(SIG_SETMASK, &mask, &prev);
        pause();
        sigprocmask(SIG_SETMASK, &prev, NULL);
        */
        sigsuspend(&mask);
    }
    return;
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.
 *     子进程终止或停止都可能触发SIGCHLD，所以我们得分类讨论。
 */
void sigchld_handler(int sig)
{
    int olderrno = errno; // 保存错误信息
    int status;
    pid_t pid;
    sigset_t mask, prev;
    sigfillset(&mask);
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) { // 立即返回该子进程的pid
        // 涉及到对全局变量jobs的访问，阻塞所有信号
        sigprocmask(SIG_BLOCK, &mask, &prev);
        struct job_t* job = getjobpid(jobs, pid);
        if (job->state == FG) { // 子进程为前台进程，打开标志
            fgpid(jobs);
        }

        if (WIFEXITED(status)) { // 正常退出，删除任务即可
            deletejob(jobs, pid);
        } else if (WIFSIGNALED(status)) { // 收到信号非正常退出，打印消息后删除任务
            printf("Job [%d] (%d) terminated by signal %d\n", job->jid, pid, WTERMSIG(status));
            deletejob(jobs, pid);
        } else if (WIFSTOPPED(status)) { // 子进程处于停止状态，切换对应的任务状态
            job->state = ST;
            printf("Job [%d] (%d) stopped by signal %d\n", job->jid, pid, WSTOPSIG(status));
        }
        sigprocmask(SIG_SETMASK, &prev, NULL);
    }
    errno = olderrno;
    return;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig)
{
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig)
{
    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t* job)
{
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t* jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t* jobs)
{
    int i, max = 0;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid > max)
            max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t* jobs, pid_t pid, int state, char* cmdline)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);
            if (verbose) {
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t* jobs, pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            nextjid = maxjid(jobs) + 1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t* jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t* getjobpid(struct job_t* jobs, pid_t pid)
{
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t* getjobjid(struct job_t* jobs, int jid)
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t* jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
            case BG:
                printf("Running ");
                break;
            case FG:
                printf("Foreground ");
                break;
            case ST:
                printf("Stopped ");
                break;
            default:
                printf("listjobs: Internal error: job[%d].state=%d ",
                    i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char* msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char* msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t* Signal(int signum, handler_t* handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}
