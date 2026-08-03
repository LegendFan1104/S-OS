#ifndef __SYSCALL_NET_H__
#define __SYSCALL_NET_H__

#include "types.h"

int sys_socket(int domain, int type, int protocol);
int sys_listen(int sockfd, int backlog);
int sys_bind(int sockfd, const uint64 addr, uint64 addrlen);
int sys_connect(int sockfd, uint64 addr, int addrlen);
int sys_accept(int sockfd, uint64 addr, uint64 addrlen_ptr);
int sys_getsockname(int sockfd, uint64 addr, uint64 addrlen_ptr);
int sys_setsockopt(int sockfd, int level, int optname, uint64 optval, uint64 optlen);
int sys_sendto(int sockfd, uint64 buf, int len, int flags, uint64 addr, int addrlen);
int sys_recvfrom(int sockfd, uint64 buf, int len, int flags, uint64 addr, uint64 addrlen_ptr);

#endif
