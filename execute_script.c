#include <malloc.h>
#include <assert.h>

#include <sys/select.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include "execute_script.h"

int execute_script(
                        const char* directory,
						const char* script_name, 
						const char*const* prepended_params,
						const char* param, 
						read_t stdin_fn, void* stdin_obj,
                        write_t stdout_fn, void* stdout_obj
						) {
    
	int ppcount=0;
	int i;
	if(prepended_params) {
		for(i=0; prepended_params[i]; ++i) ++ppcount;	
	}
	const char** argv = (const char**)malloc((ppcount+3)*sizeof(char*));
	assert(argv!=NULL);
	
	char script_path[4096];
	
	sprintf(script_path, "%s/%s", directory, script_name);

	argv[0]=script_name;
	argv[1]=param;
	for(i=0; i<ppcount; ++i) argv[i+2] = prepended_params[i];
	argv[ppcount+2]=NULL;
	
	int child_stdout = -1;
	int child_stdin = -1;
	
	int to_be_written = -1;
	int to_be_read = -1;
	
	if(stdin_fn)
	   {int p[2]; int ret = pipe(p); assert(ret==0); to_be_written=p[1]; child_stdin =p[0];}
	if(stdout_fn)
	   {int p[2]; int ret = pipe(p); assert(ret==0); to_be_read   =p[0]; child_stdout=p[1];}
	
	
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGPIPE);
    int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK);
    assert(signal_fd!=-1);
    sigprocmask(SIG_BLOCK, &mask, NULL);
        
	int childfd = fork();
	if(!childfd) {
	   close(signal_fd);
	   if(child_stdout!=-1) {
	       close(to_be_read);
	       dup2(child_stdout, 1);
	       close(child_stdout);
	   }
	   if(child_stdin!=-1) {
	       close(to_be_written);
	       dup2(child_stdin, 0);
	       close(child_stdin);
	   }
	   execv(script_path, (char**)argv);
	   _exit(127);
	}
	
    if(child_stdout!=-1) {
        close(child_stdout);
    }
    if(child_stdin!=-1) {
        close(child_stdin);
    }
	
    /* Event loop for feeding the script with input data, reading output and reading exit code */
    
    int maxfd = signal_fd;
    if(to_be_written!=-1 && maxfd<to_be_written) maxfd = to_be_written;
    if(to_be_read!=-1    && maxfd<to_be_read   ) maxfd = to_be_read   ;
    ++maxfd;
    
    char buf[65536];
    fd_set rfds;
    fd_set wfds;
    
    int exit_code=127;
    
    for(;;) {
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(signal_fd, &rfds);
        if(to_be_read!=-1) FD_SET(to_be_read, &rfds);
        if(to_be_written!=-1) FD_SET(to_be_written, &wfds);
        
        int ret = select(maxfd+1, &rfds, &wfds, NULL, NULL);
        
        if (ret == -1) {
            if (errno==EINTR || errno==EAGAIN) continue;
            break;
        }
        
        if (to_be_read!=-1 && FD_ISSET(to_be_read, &rfds)) {
            ret = read(to_be_read, buf, sizeof buf);
            if (ret==-1) {
                if (errno == EINTR || errno==EAGAIN) continue;
            }
            if (ret==0 || ret == -1) {
                close(to_be_read);
                to_be_read = -1;
            } else {
                int ret2 = (*stdout_fn)(stdout_obj, buf, ret);
                if (ret2<ret) {
                    close(to_be_read);
                    to_be_read = -1;
                } else {
                    continue; // Don't allow handling of SIGCHLD before reading all the data
                }
            }
        }
        
        if (to_be_written!=-1 && FD_ISSET(to_be_written, &wfds)) {
            ret = (*stdin_fn)(stdin_obj, buf, sizeof buf);
            if (ret==-1) {
                if (errno == EINTR || errno==EAGAIN) continue;
            }
            if(!ret || ret == -1) {
                close(to_be_written);
                to_be_written=-1;
            } else {
                ret = write(to_be_written, buf, ret);
                if (ret==-1) {
                    if (errno == EINTR || errno==EAGAIN) continue;
                }
                if(ret==0 || ret == -1) {
                    close(to_be_written);
                    to_be_read = -1;
                }
            }
        }
        
        if (FD_ISSET(signal_fd, &rfds)) {
            struct signalfd_siginfo si;
            if (read(signal_fd, &si, sizeof si)>0) {
                if(si.ssi_signo == SIGCHLD) {
                    exit_code = si.ssi_status;
                    break;
                }
            }                
        }
    }
	
    close(signal_fd);
    if(to_be_read!=-1) close(to_be_read);
    if(to_be_written!=-1) close(to_be_read);
    
	free(argv);
	return exit_code;
}