#include "proc/socket.h"

#include "proc/proc.h"
#include "mem/kalloc.h"
#include "lib/string.h"
#include "sys/fcntl.h"


static uint socket_bitmap[SOCKET_COUNT / 32] = {0};
struct Socket sockets[SOCKET_COUNT];
struct spinlock socketlock;

struct message messages[MESSAGE_COUNT];
msg_list message_free_list;
struct spinlock messageslock;

//初始化socket锁和相关结构
void socket_init()
{
    int i;
    initlock(&socketlock, "socketlock");
    initlock(&messageslock, "messageslock");

    for (i = 0; i < SOCKET_COUNT; i++) {
        initlock(&sockets[i].lock, "socketlock");
        initlock(&sockets[i].state.state_lock, "socketstatelock");
        TAILQ_INIT(&sockets[i].messages);
    }

    // init free message queue
    TAILQ_INIT(&message_free_list);
    for (i = 0; i < MESSAGE_COUNT; i++)
        TAILQ_INSERT_TAIL(&message_free_list, &messages[i], message_link);
}

static message * message_alloc() {
    message * message;

    acquire(&messageslock);
    if (TAILQ_EMPTY(&message_free_list)) {
        printf("Alloc Error\n");
        release(&messageslock);
        return NULL;
    }


    message = TAILQ_FIRST(&message_free_list);
    TAILQ_REMOVE(&message_free_list, message, message_link);
    release(&messageslock);

    message->message_link.tqe_next= NULL;
    message->message_link.tqe_prev = NULL;
    message->bufferAddr = kmalloc(PGSIZE);
    message->length = 0;

    return message;
}


int socketalloc()
{
    int i;
    acquire(&socketlock);
    for (i = 0; i < SOCKET_COUNT; i++) {
        int index = i >> 5;
        int inner = i & 31;
        if ((socket_bitmap[index] & (1 << inner)) == 0) {
            socket_bitmap[index] |= 1 << inner;
            release(&socketlock);
            return i;
        }
    }
    release(&socketlock);
    return -1;
}


int socket(struct file *f, int domain, int type, int protocol)
{
    int socketnum = socketalloc();
    if (socketnum < 0)
        return -1;

    struct Socket *s = &sockets[socketnum];
    acquire(&s->lock);
    s->used = true;
    s->addr.family = domain;
    s->type = (type & 0xf);
    memset(&s->target_addr, 0, sizeof(socket_addr));
    s->waiting_h = s->waiting_t = 0;
    s->bufferAddr = (void *)kmalloc(SOCKET_BUFFER_SIZE);
    s->pid = myproc()->pid;
    s->udp_is_connect = 0;
    s->opposite = -1;

    TAILQ_INIT(&s->messages);

    acquire(&s->state.state_lock);
    s->state.is_close = false;
    s->state.opposite_write_close = false;
    release(&s->state.state_lock);
    release(&s->lock);

    f->f_type = FD_SOCKET;
    uint64 other_flags = type & ~SOCKET_TYPE_MASK;
    f->f_socketflags = O_RDWR | other_flags;
    f->f_pos = 0;
    f->f_socket = s;
    f->f_socketnum = socketnum;

    return 0;
}


int listen(struct Socket *s, int backlog)
{
    acquire(&s->lock);
    s->listening = 1;
    release(&s->lock);

    return 0;
}

static void gen_local_socket_addr(socket_addr * socket_addr)
{
    static uint32 local_addr = (127 << 24) + 1;
    static uint32 local_port = 10000;
    socket_addr->addr = local_addr;
    socket_addr->port = local_port++;
}

int bind(struct Socket *s, const socket_addr *p_sockectaddr, uint32 addrlen)
{
    socket_addr socketaddr;
    if(memmove((void*)&socketaddr, (const void*)&p_sockectaddr, sizeof(socket_addr)) < 0)
        return -1;

    acquire(&s->lock);
    if (s->addr.family == socketaddr.family) {
        s->addr.addr = socketaddr.addr;
        s->addr.port = socketaddr.port;
    }
    release(&s->lock);

    return 0;
}

static Socket *find_udp_connect_socket(socket_addr *addr, int self_type, int socket_index)
{
    for (int i = 0; i < SOCKET_COUNT; ++i) {
        acquire(&sockets[i].lock);
        if (sockets[i].used &&
          i != socket_index &&
          sockets[i].type == self_type &&
          (sockets[i].addr.port == addr->port) &&
          (!sockets[i].udp_is_connect || (sockets[i].udp_is_connect && sockets[i].opposite == socket_index))
        ) {
            release(&sockets[i].lock);
            return &sockets[i];
        }
        release(&sockets[i].lock);
    }
    return NULL;
}

static Socket *find_listening_socket(const socket_addr *addr, int type)
{
    for (int i = 0; i < SOCKET_COUNT; i++) {
        acquire(&sockets[i].lock);
        if (sockets[i].used &&
          (sockets[i].type & SOCKET_TYPE_MASK) == type &&
          sockets[i].addr.port == addr->port &&
          sockets[i].listening
        ) {
            return &sockets[i];
        }
        release(&sockets[i].lock);
    }
    return NULL;
}

int connect(struct Socket *ls, const socket_addr *p_addr, uint32 addrlen)
{
    socket_addr addr;
    if(copyin(myproc()->pagetable, (char*)&addr, (uint64)p_addr, sizeof(socket_addr)) < 0)
        return -1;

    acquire(&ls->lock);
    if (ls->addr.port == 0)
        gen_local_socket_addr(&ls->addr);
    ls->target_addr = addr;
    release(&ls->lock);

    // 如果是UDP，只设置target_addr就可以
    if (SOCK_IS_UDP(ls->type)) {
        Socket *ts = find_udp_connect_socket(&addr, ls->type & SOCKET_TYPE_MASK, ls - sockets);
        if (ts == NULL) {
            printf("server socket doesn't exists or isn't listening!\n");
            return 0;
        }
        acquire(&ls->lock);
        ls->udp_is_connect = 1;
        ls->opposite = ts - sockets;
        release(&ls->lock);
        printf("socket connect: type = UDP\n");
        return 0;
    }

    Socket *ts = find_listening_socket(&addr, ls->type & SOCKET_TYPE_MASK);
    if (ts == NULL) {
        printf("server socket doesn't exists or isn't listening!\n");
        return -1;
    }

    if (ts->waiting_t - ts->waiting_h == PENDING_COUNT) {
        printf("target socket's pending queue is full\n");
        return -1; // 达到最高限制
    }

    ts->waiting_queue[(ts->waiting_t++) % PENDING_COUNT] = ls->addr;

    int pos = (ts->waiting_t - 1 + PENDING_COUNT) % PENDING_COUNT;

    wakeup(ts->waiting_queue); // 尝试唤醒服务端，以等待队列指针作为chan

    if (ls->pid != ts->pid) // 释放服务端target_socket的锁，客户端进入睡眠，等待服务端唤醒客户端
        sleep(&ts->waiting_queue[pos], &ts->lock);

    release(&ts->lock);

    return 0;
}

static void message_free(message *m) {
    m->message_link.tqe_next = NULL;
    m->message_link.tqe_prev = NULL;
    m->family = 0;
    m->port = 0;
    m->addr = 0;
    m->length = 0;
    kfree((void*)m->bufferAddr);

    acquire(&messageslock);
    TAILQ_INSERT_TAIL(&message_free_list, m, message_link);
    release(&messageslock);
}

void socketfree(int socketnum)
{
    Socket *s = &sockets[socketnum];

    acquire(&s->lock);
    if (s->listening != 0)
        printf("closing listening socket %d!\n", socketnum);
    s->used = false;
    s->socketReadPos = 0;
    s->socketWritePos = 0;
    s->listening = 0;
    s->waiting_h = 0;
    s->waiting_t = 0;
    s->addr.family = 0;
    s->type = 0;

    if (s->bufferAddr != NULL) {
        kfree(s->bufferAddr);
        s->bufferAddr = NULL;
    }

    memset(&s->addr, 0, sizeof(socket_addr));
    memset(&s->target_addr, 0, sizeof(socket_addr));
    memset(s->waiting_queue, 0, (sizeof(socket_addr) * PENDING_COUNT));

    message *m;
    while (!TAILQ_EMPTY(&s->messages)) {
        m = TAILQ_FIRST(&s->messages);
        TAILQ_REMOVE(&s->messages, m, message_link);
        message_free(m);
    }

    acquire(&s->state.state_lock);
    s->state.is_close = false;
    release(&s->state.state_lock);

    release(&s->lock);

    acquire(&socketlock);
    if(!(socketnum >= 0 && socketnum < SOCKET_COUNT))
        panic("socketfree");
    int index = socketnum >> 5, inner = socketnum & 31;
    socket_bitmap[index] &= ~(1 << inner);
    release(&socketlock);
}

static Socket *remote_find_peer_socket(const Socket *ls) {
    for (int i = 0; i < SOCKET_COUNT; ++i) {
        acquire(&sockets[i].lock);
        if (sockets[i].used &&
          ls - sockets != i && // not self
          sockets[i].addr.port == ls->target_addr.port &&
          sockets[i].target_addr.port == ls->addr.port
          ) {
            return &sockets[i];
          }
        release(&sockets[i].lock);
    }
    return NULL;
}

void socketclose(struct Socket *ls, int socketnum)
{
    Socket *ts = remote_find_peer_socket(ls);
    if (ts != NULL) {
        acquire(&ts->state.state_lock);
        ts->state.is_close = true;
        release(&ts->state.state_lock);
        wakeup(&ts->socketReadPos);
        release(&ts->lock); // is right?
    }

    wakeup(&ls->socketWritePos);
    // int socketnum = ls - sockets;
    socketfree(socketnum);
}

int checkaccept(uint32 socketflag, struct Socket *ls)
{
    acquire(&ls->lock);

    // 如果是无阻塞模式下的socket，那么直接返回即可
    if ((socketflag & O_NONBLOCK) && ls->waiting_h == ls->waiting_t) {
        release(&ls->lock);
        return -1;
    }

    while (ls->waiting_h == ls->waiting_t)
        // 此时代表没有连接请求，释放服务端socket锁，进入睡眠
            // 等到客户端有请求时，唤醒服务端，并且被唤醒后，此时服务端拥有服务端socket的锁
                sleep(ls->waiting_queue, &ls->lock);
    return 0;
}

void accept(struct Socket *ls, struct Socket *ns, socket_addr *p_addr, uint32 *addrlen)
{
    socket_addr addr;
    // Socket *addr = socket->waiting_queue[(socket->waiting_h++) % PENDING_COUNT];

    ns->addr = ls->addr;
    ns->target_addr = ls->waiting_queue[(ls->waiting_h++) % PENDING_COUNT];
    addr = ns->target_addr;

    // 释放服务端socket锁，唤醒newsocket对应的客户端
    int pos = (ls->waiting_h - 1 + PENDING_COUNT) % PENDING_COUNT;
    wakeup(&ls->waiting_queue[pos]);
    release(&ls->lock);

    copyout(myproc()->pagetable, (uint64)p_addr, (char*)&addr, sizeof(socket_addr));
    printf("accept socket addr = %x, port = %d\n", addr.addr, addr.port);
}

int getsockname(struct Socket *ls, socket_addr *addr, uint32 *addrlen)
{
    uint32 len = sizeof(socket_addr);

    if (addr)
        copyout(myproc()->pagetable, (uint64)addr, (char*)&ls->addr, sizeof(socket_addr));
    if (addrlen)
        copyout(myproc()->pagetable, (uint64)addrlen, (char*)&len, sizeof(len));
    return 0;
}

int getpeername(struct Socket *ls, socket_addr *addr, uint32 *addrlen)
{
    uint32 len = sizeof(socket_addr);

    if (addr)
        copyout(myproc()->pagetable, (uint64)addr, (char*)&ls->target_addr, sizeof(socket_addr));
    if (addrlen)
        copyout(myproc()->pagetable, (uint64)addrlen, (char*)&len, sizeof(len));
    return 0;
}

int getsockopt(struct Socket *ls, int lever, int optname, void *optval, uint32 *optlen)
{
  int val, size = 4;

  if (lever == SOL_SOCKET) {
    if (optname == SO_RCVBUF) {
      val = 131072;
      copyout(myproc()->pagetable, (uint64)optval, (char*)&val, sizeof(uint32));
    } else if (optname == SO_SNDBUF) {
      val = 16384;
      copyout(myproc()->pagetable, (uint64)optval, (char*)&val, sizeof(uint32));
    }
    copyout(myproc()->pagetable, (uint64)optlen, (char*)&size, sizeof(uint32));
  }
  return 0;
}

int setsockopt(struct Socket *ls, int lever, int optname, const void *optval, uint32 *optlen)
{
  return 0; // TODO:
}

static int fd_socket_write(struct file *f, uint64 buf, uint64 n, uint64 offset) {
  uint64 begin_time = rdtime();
  Socket *ls = f->f_socket;
  int i = 0;

  if (SOCK_IS_UDP(ls->type)) // UDP
    return sendto(f, (void *)buf, n, 0, &ls->target_addr, NULL, 0);

  // TCP
  acquire(&ls->lock);
  if (ls->self_write_close) {
    release(&ls->lock);
    return -1;
  }
  release(&ls->lock);

  Socket *ts = remote_find_peer_socket(ls);
  if (ts == NULL || ts->self_read_close) { // 可能是远端已关闭，或者远端关闭读
    printf("socket write error: cant find target socket.\n");
    return -1;
  }

  acquire(&ls->state.state_lock); // 获得自身socket的状态锁，从而来获得ts是否关闭的状态

  while (i < n) {
    if (ls->state.is_close /* 对面socket进程已结束*/) {
      if (i == 0)	{
        printf("socket write error: target socket is closed.\n");
        release(&ls->state.state_lock);
        release(&ts->lock);
        return -1;
      } else {
        printf("socket writer can\'t write more.\n");
        release(&ls->state.state_lock);
        break;
      }
    }
    // 对端肯定没有关闭，但不一定没有关闭读，此时对面有可能已经关闭了读
    if (ts->self_read_close) {
      printf("socket writer can\'t write more because target is close reader\n");
      release(&ls->state.state_lock);
      break;
    } else {
      if (ts->socketWritePos - ts->socketReadPos == SOCKET_BUFFER_SIZE) {
        release(&ls->state.state_lock);

        wakeup(&ts->socketReadPos);
        // printf("[%ld] write sleep, wait socket to read\n", time_rtc_us());
        uint64 _start = rdtime();
        sleep(&ts->socketWritePos, &ts->lock);
        // printf("[%ld] write wakeup, wait socket to read\n", time_rtc_us());
        begin_time += (rdtime() - _start);

        acquire(&ls->state.state_lock);
      } else {
        uint64 left_size = SOCKET_BUFFER_SIZE - (ts->socketWritePos - ts->socketReadPos);
        uint64 write_length = MIN(left_size, n);
        uint64 write_dst = ts->socketWritePos + write_length;

        uint64 write_begin = ts->socketWritePos % SOCKET_BUFFER_SIZE;
        uint64 write_end = write_dst % SOCKET_BUFFER_SIZE;

        if (write_begin < write_end) {
          copyin(myproc()->pagetable, (char*)(ts->bufferAddr + write_begin), (uint64)(buf + i), write_length);
        } else {
          copyin(myproc()->pagetable, (char*)(ts->bufferAddr + write_begin), (uint64)(buf + i), SOCKET_BUFFER_SIZE - write_begin);
          copyin(myproc()->pagetable, (char*)ts->bufferAddr, (uint64)(buf + i + SOCKET_BUFFER_SIZE - write_begin),  write_end);
        }
        i += write_length;
        ts->socketWritePos += write_length;
      }
    }
  }

  release(&ls->state.state_lock);

  f->f_pos += i;
  wakeup(&ts->socketReadPos);
  release(&ts->lock);
  return i;
}

static Socket * find_udp_remote_socket(socket_addr * addr, int self_type, int socket_index) {
  for (int i = 0; i < SOCKET_COUNT; ++i) {
    acquire(&sockets[i].lock);
    if (sockets[i].used &&
      i != socket_index &&
      sockets[i].type == self_type &&
      (sockets[i].addr.port == addr->port &&(!sockets[i].udp_is_connect || (sockets[i].udp_is_connect && sockets[i].opposite == socket_index))) &&
      (!sockets[i].udp_is_connect || (sockets[i].udp_is_connect && sockets[i].opposite == socket_index))
    ) {
      release(&sockets[i].lock);
      return &sockets[i];
    }
    release(&sockets[i].lock);
  }
  return NULL;
}

int sendto(struct file *f, const void *buffer, uint64 len, int flags, const socket_addr *dst_addr, uint32 *addrlen, int user) {
  struct Socket *ls = f->f_socket;
  if (user) {
    if (ls == NULL)
      asm volatile("nop");
    if (ls->type == 1)
      return fd_socket_write(f, (uint64)buffer, (uint64)len, 0);
  }

  socket_addr socketaddr;
  if (user)
    copyin(myproc()->pagetable, (char*)&socketaddr, (uint64)dst_addr, sizeof(socket_addr));
  else
    socketaddr = *dst_addr;

  Socket *ts = find_udp_remote_socket(&socketaddr, ls->type, ls - sockets);

  if (ts == NULL) {
    printf("target addr socket doesn't exists\n");
    return MIN(len, 65535);
  }

  struct message *message = message_alloc();
  if (message == NULL)
    return -1;

  message->family = ls->addr.family;
  message->addr = ls->addr.addr;
  message->port = ls->addr.port;

  int min_len = MIN(len, 65535);
  // copyin(myproc()->pagetable, (char*)message->bufferAddr, (uint64)buffer, min_len);
  memmove(message->bufferAddr, buffer, min_len);
  message->length = min_len;

  acquire(&ts->lock);
  TAILQ_INSERT_TAIL(&ts->messages, message, message_link);
  wakeup(&ts->messages); // 唤醒对面的recvfrom
  release(&ts->lock);

  return min_len;
}

static int fd_socket_read(struct file *f, uint64 buf, uint64 n, uint64 offset) {
  Socket *ls = f->f_socket;

  if (SOCK_IS_UDP(ls->type))
    return recvfrom(f, (void *)buf, n, 0, &ls->target_addr, NULL, 0);

  acquire(&ls->lock);

  if (ls->self_read_close) {
    release(&ls->lock);
    return -1;
  }
  while (ls->socketReadPos == ls->socketWritePos) {
    acquire(&ls->state.state_lock);
    if (!ls->state.is_close && !ls->state.opposite_write_close) {
      release(&ls->state.state_lock);
      wakeup(&ls->socketWritePos);
      // printf("[%ld] read sleep, wait socket to write\n", time_rtc_us());
      sleep(&ls->socketReadPos, &ls->lock);
      // printf("[%ld] read wakeup, wait socket to write\n", time_rtc_us());
    } else {
      release(&ls->state.state_lock);
      printf("target has closed or target writer is closed.");
      break;
    }
  }

  uint64 socket_volumn = ls->socketWritePos - ls->socketReadPos; // 实际容量
  uint64 read_volumn = MIN(n, socket_volumn);
  uint64 read_dst = ls->socketReadPos + read_volumn;

  // 读取数据
  uint64 read_begin = ls->socketReadPos % SOCKET_BUFFER_SIZE;
  uint64 read_end = read_dst % SOCKET_BUFFER_SIZE;

  if (read_volumn != 0) {
    if (read_begin < read_end) {
      copyout(myproc()->pagetable, (uint64)buf, (char*)(ls->bufferAddr + read_begin), read_volumn);
    } else {
      copyout(myproc()->pagetable, (uint64)buf, (char*)(ls->bufferAddr + read_begin), SOCKET_BUFFER_SIZE - read_begin);
      copyout(myproc()->pagetable, (uint64)(buf + SOCKET_BUFFER_SIZE - read_begin), (char*)ls->bufferAddr, read_end);
    }
  }

  ls->socketReadPos += read_volumn;
  f->f_pos += read_volumn;

  wakeup(&ls->socketWritePos);
  release(&ls->lock);

  return read_volumn;
}

int recvfrom(struct file *f, void *buffer, uint64 len, int flags, socket_addr *src_addr, uint32 *addrlen, int user) {
  int min_size;
  struct Socket *ls = f->f_socket;

  if (user && ls->type == 1)
    return fd_socket_read(f, (uint64)buffer, (uint64)len, 0);

  socket_addr socketaddr;
  if (!user)
    socketaddr = *src_addr;

  message *message = NULL, *msg;
  acquire(&ls->lock);
  while (message == NULL) {
    if (!user) {
      TAILQ_FOREACH(msg, &ls->messages, message_link) {
        if (msg->port == socketaddr.port) {
          message = msg;
          break;
        }
      }
    } else {
      message = TAILQ_FIRST(&ls->messages);
    }

    if (message == NULL) {
      sleep(&ls->messages, &ls->lock); // TODO: wakeup
    } else {
      min_size = MIN(len, message->length);
      // copyout2((uint64)buffer, (char*)message->bufferAddr, min_size);
      memmove(buffer, message->bufferAddr, min_size);

      // 向用户态返回对方地址
      socketaddr.family = message->family;
      socketaddr.port = message->port;
      socketaddr.addr = message->addr;
      if (user && src_addr)
        // copyout2((uint64)src_addr, (char*)&socketaddr, sizeof(socket_addr));
        memmove(src_addr, &socketaddr, sizeof(socket_addr));
      break;
    }
  }
  TAILQ_REMOVE(&ls->messages, message, message_link);
  release(&ls->lock);

  message_free(message);
  return min_size;
}

int shutdown(struct Socket *ls, int how)
{
	if (how == SHUT_RD) {
		acquire(&ls->lock);
		ls->self_read_close = true;
		wakeup(&ls->socketWritePos);
		release(&ls->lock);
	} else if (how == SHUT_WR) {
		Socket *ts = remote_find_peer_socket(ls);
		if (ts != NULL) {
			acquire(&ts->state.state_lock);
			ts->state.opposite_write_close = true;
			release(&ts->state.state_lock);
			wakeup(&ts->socketReadPos);
			release(&ts->lock);
		}
		acquire(&ls->lock);
		ls->self_write_close= true;
		release(&ls->lock);
	} else if (how == SHUT_RDWR) {
		Socket *ts = remote_find_peer_socket(ls);
		if (ts != NULL) {
			acquire(&ts->state.state_lock);
			ts->state.opposite_write_close = true;
			release(&ts->state.state_lock);
			wakeup(&ts->socketReadPos);
			release(&ts->lock);
		}
		acquire(&ls->lock);
		ls->self_write_close= true;
		ls->self_read_close = true;
		wakeup(&ls->socketWritePos);
		release(&ls->lock);
	} else
		return -1;
	return 0;
}










