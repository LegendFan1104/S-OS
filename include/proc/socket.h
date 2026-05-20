#pragma once


#include "types.h"
#include "platform.h"
#include "lock/spinlock.h"
#include "fs/ext4/lwext4/misc/queue.h"
#include "fs/vfs/file.h"


#define SOCKET_COUNT        128
#define PENDING_COUNT       128
#define MESSAGE_COUNT       512
#define SOCKET_BUFFER_SIZE  (PGSIZE * 32)

#define AF_UNIX             1
#define AF_LOCAL            1
#define AF_INET             2
#define AF_INET6            10

#define SOCK_STREAM         1
#define SOCK_DGRAM          2

#define SOL_SOCKET          1
#define SO_RCVBUF           8
#define SO_SNDBUF           7

#define SHUT_RD             0
#define SHUT_WR             1
#define SHUT_RDWR           2

typedef struct socket_addr {
  uint16 family;
  uint16 port;
  uint32 addr;
  char zero[8];
} socket_addr;

typedef struct socket_state {
  struct spinlock state_lock;
  bool is_close;
  bool opposite_write_close;
} socket_state;

typedef	struct message {
  TAILQ_ENTRY(message) message_link;
  uint16 family; // sender's family
  uint16 port; // sender's port
  uint32 addr; // sender's address
  void* bufferAddr;
  uint64 length;
} message;

typedef TAILQ_HEAD(msg_list, message) msg_list;

typedef struct Socket {
  bool used;
  struct spinlock lock;
  uint32 type;
  socket_addr addr;
  socket_addr target_addr;
  uint64 socketReadPos;
  uint64 socketWritePos;
  socket_addr waiting_queue[PENDING_COUNT];
  int waiting_h;
  int waiting_t;
  int listening;
  void *bufferAddr;
  socket_state state;
  uint64 pid;
  bool self_read_close;
  bool self_write_close;
  msg_list messages;
  int udp_is_connect;
  int opposite;
} Socket;

void            socket_init();
int socket(struct file *f, int domain, int type, int protocol);
int             bind(struct Socket *, const socket_addr *, uint32);
int             listen(struct Socket *, int);
int             connect(struct Socket *, const socket_addr *, uint32);
int             checkaccept(uint32, struct Socket *);
void            accept(struct Socket *, struct Socket *, socket_addr *, uint32 *);
void            socketfree(int);
void            socketclose(struct Socket *, int);

int             getsockopt(struct Socket *, int, int, void *, uint32 *);
int             setsockopt(struct Socket *, int, int, const void *, uint32 *);
int             getsockname(struct Socket *, socket_addr *, uint32 *);
int             sendto(struct file *, const void *, uint64, int, const socket_addr *, uint32 *, int);
int             recvfrom(struct file *, void *, uint64, int, socket_addr *, uint32 *, int);
int             getpeername(struct Socket *, socket_addr *, uint32 *);
int             shutdown(struct Socket *, int);

#define SOCKET_TYPE_MASK 0xf
#define SOCK_IS_UDP(type) (((type) & SOCKET_TYPE_MASK) == SOCK_DGRAM)

