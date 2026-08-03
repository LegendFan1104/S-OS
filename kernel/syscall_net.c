#include "types.h"
#include "print.h"
#include "defs.h"
#include "vmem.h"
#include "process.h"
#include "string.h"
#include "fcntl.h"
#include "file.h"
#include "socket.h"
#include "errno-base.h"
#include "pmem.h"
#include "slab.h"
#include "syscall_net.h"


/**
 * @brief 分配一个socket描述符
 *
 * @param domain 指定通信发生的区域
 * @param type   描述要建立的套接字的类型 SOCK_STREAM:流式   SOCK_DGRAM：数据报式
 * @param protocol 该套接字使用的特定协议
 * @return int
 */
static struct socket *
find_listening_socket(uint16 port)
{
    extern struct proc pool[NPROC];
    int i, j;

    for (i = 0; i < NPROC; i++)
    {
        proc_t *p = &pool[i];
        if (p->state == UNUSED)
            continue;
        for (j = 0; j < NOFILE; j++)
        {
            struct file *f = p->ofile[j];
            if (f == 0 || f->f_type != FD_SOCKET || f->f_data.sock == 0)
                continue;
            if (f->f_data.sock->state != SOCKET_LISTENING)
                continue;
            if (f->f_data.sock->local_addr.sin_port != port)
                continue;
            return f->f_data.sock;
        }
    }
    return 0;
}

static int
alloc_connected_socket_fd(struct socket *listener, struct sockaddr_in *remote_addr)
{
    struct file *f = filealloc();
    struct socket *sock;
    int fd;

    if (!f)
        return -ENFILE;
    sock = kalloc();
    if (!sock)
    {
        f->f_count = 0;
        return -ENOMEM;
    }

    memset(sock, 0, sizeof(*sock));
    sock->domain = listener->domain;
    sock->type = listener->type;
    sock->protocol = listener->protocol;
    sock->state = SOCKET_CONNECTED;
    sock->local_addr = listener->local_addr;
    sock->remote_addr = *remote_addr;

    f->f_type = FD_SOCKET;
    f->f_flags = O_RDWR;
    f->f_data.sock = sock;

    fd = fdalloc(f);
    if (fd < 0)
    {
        f->f_count = 0;
        return -EMFILE;
    }
    return fd;
}

int sys_socket(int domain, int type, int protocol)
{
    DEBUG_LOG_LEVEL(LOG_INFO, "[sys_socket] domain: %d, type: %d, protocol: %d\n", domain, type, protocol);
    int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    ///< SOCK_CLOEXEC 设置文件描述符的close-on-exec，自动关闭文件描述符
    ///< SOCK_NONBLOCK 将socket设置为非阻塞，需通过轮询或事件驱动
    type &= ~(SOCK_CLOEXEC | SOCK_NONBLOCK);

    if (domain != PF_INET)
        return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -ESOCKTNOSUPPORT;
    if (protocol != 0)
    {
        if (type == SOCK_STREAM && protocol != IPPROTO_TCP)
            return -EPROTONOSUPPORT;
        if (type == SOCK_DGRAM && protocol != IPPROTO_UDP)
            return -EPROTONOSUPPORT;
    }

    struct file *f;
    f = filealloc();
    if (!f)
        return -ENFILE;

    struct socket *sock = kalloc();
    if (!sock)
    {
        get_file_ops()->close(f);
        return -ENOMEM;
    }
    memset(sock, 0, sizeof(struct socket));
    sock->domain = domain;
    sock->type = type;
    sock->protocol = protocol ? protocol : (type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    sock->state = SOCKET_UNBOUND; ///< 分配时将socket状态设置为未绑定

    if (flags & SOCK_CLOEXEC)
        f->f_flags |= O_CLOEXEC;
    if (flags & SOCK_NONBLOCK)
        f->f_flags |= O_NONBLOCK;
    f->f_type = FD_SOCKET; ///< 设置文件类型为socket
    f->f_flags |= O_RDWR;
    f->f_data.sock = sock;

    int fd = -1;
    if ((fd = fdalloc(f)) == -1)
    {
        get_file_ops()->close(f);
        return -EMFILE;
    };
    return fd;
}

int sys_listen(int sockfd, int backlog)
{
    DEBUG_LOG_LEVEL(LOG_INFO, "[sys_listen] sockfd: %d, backlog: %d\n", sockfd, backlog);
    proc_t *p = myproc();
    struct file *f;

    (void)backlog;
    if (sockfd < 0 || sockfd >= NOFILE || (f = p->ofile[sockfd]) == 0)
        return -EBADF;
    if (f->f_type != FD_SOCKET)
        return -ENOTSOCK;
    if (f->f_data.sock->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (f->f_data.sock->state == SOCKET_UNBOUND)
    {
        struct sockaddr_in local_addr = {
            .sin_family = PF_INET,
            .sin_addr = INADDR_ANY,
            .sin_port = 0,
        };
        int ret = sock_bind(f->f_data.sock, &local_addr, sizeof(local_addr));
        if (ret < 0)
            return ret;
    }

    f->f_data.sock->state = SOCKET_LISTENING;
    return 0;
}

/**
 * @brief 绑定套接字到本地地址
 *
 * @param sockfd 套接字描述符
 * @param addr 地址结构指针
 * @param addrlen 地址结构长度
 * @return int 状态码
 */
int sys_bind(int sockfd, const uint64 addr, uint64 addrlen)
{
    DEBUG_LOG_LEVEL(LOG_INFO, "[sys_bind] sockfd: %d, addr: %p, addrlen: %d\n", sockfd, addr, addrlen);
    proc_t *p = myproc();
    struct sockaddr_in socket;
    struct file *f;
    if (sockfd < 0 || sockfd >= NOFILE || (f = p->ofile[sockfd]) == 0)
        return -EBADF;
    if (f->f_type != FD_SOCKET)
        return -ENOTSOCK;
    if (addrlen < sizeof(socket))
        return -EINVAL;
    if (copyin(p->pagetable, (char *)&socket, addr, sizeof(socket)) == -1)
        return -EFAULT;
    // 验证地址族
    if (socket.sin_family != PF_INET)
        return -EAFNOSUPPORT;
    struct socket *sock = f->f_data.sock;
    // 调用内部绑定函数
    int ret = sock_bind(sock, (struct sockaddr_in *)&socket, addrlen);
    if (ret < 0)
        return ret;
    if (copyout(p->pagetable, addr, (char *)&socket, sizeof(socket)) == -1)
        return -EFAULT;
    return 0;
};

/**
 * @brief 将套接字连接到目标地址
 *
 * @param sockfd 套接字描述符
 * @param addr 目标地址指针
 * @param addrlen 地址结构长度
 * @return int 成功返回0，失败返回错误码
 */
int sys_connect(int sockfd, uint64 addr, int addrlen)
{
    DEBUG_LOG_LEVEL(LOG_INFO, "[sys_connect] sockfd: %d, addr: %p, addrlen: %d\n",
                    sockfd, addr, addrlen);

    struct proc *p = myproc();
    struct file *f;
    struct sockaddr_in dest_addr;

    // 获取文件描述符对应的文件结构
    if ((f = p->ofile[sockfd]) == 0)
    {
        DEBUG_LOG_LEVEL(LOG_ERROR, "Invalid socket fd: %d\n", sockfd);
        return -EBADF;
    }

    // 检查文件类型是否为套接字
    if (f->f_type != FD_SOCKET)
    {
        DEBUG_LOG_LEVEL(LOG_ERROR, "fd %d is not a socket\n", sockfd);
        return -ENOTSOCK;
    }

    // 检查套接字状态（未绑定则隐式绑定）
    if (f->f_data.sock->state < SOCKET_BOUND)
    {
        struct sockaddr_in local_addr = {
            .sin_family = PF_INET,
            .sin_addr = INADDR_ANY,
            .sin_port = 2000 // 默认端口
        };

        int ret = sock_bind(f->f_data.sock, &local_addr, sizeof(local_addr));
        if (ret < 0)
        {
            DEBUG_LOG_LEVEL(LOG_ERROR, "Implicit bind failed: %d\n", ret);
            return ret;
        }
    }

    // 验证地址长度
    if (addrlen < sizeof(struct sockaddr_in))
    {
        DEBUG_LOG_LEVEL(LOG_ERROR, "Address length too small: %d < %d\n",
                        addrlen, sizeof(struct sockaddr_in));
        return -EINVAL;
    }

    // 从用户空间复制地址信息
    if (copyin(p->pagetable, (char *)&dest_addr, addr, sizeof(dest_addr)))
    {
        DEBUG_LOG_LEVEL(LOG_ERROR, "Failed to copy address from user\n");
        return -EFAULT;
    }

    // 验证地址族
    if (dest_addr.sin_family != PF_INET)
        return -EAFNOSUPPORT;

    // 设置远程地址并更新状态
    memcpy(&f->f_data.sock->remote_addr, &dest_addr, sizeof(dest_addr));
    if (f->f_data.sock->type == SOCK_STREAM)
    {
        struct socket *listener = find_listening_socket(dest_addr.sin_port);

        if (listener)
        {
            listener->pending_conn = 1;
            listener->pending_remote_addr = f->f_data.sock->local_addr;
        }
    }
    f->f_data.sock->state = SOCKET_CONNECTED;

    DEBUG_LOG_LEVEL(LOG_DEBUG, "Connected to %08x:%d\n",
                    dest_addr.sin_addr, dest_addr.sin_port);
    return 0;
}

/**
 * @brief 接受套接字上的新连接
 *
 * @param sockfd 监听套接字的文件描述符
 * @param addr 用于存储客户端地址的指针
 * @param addrlen_ptr 指向地址长度变量的指针
 * @return int 新套接字的文件描述符或错误码
 */
int sys_accept(int sockfd, uint64 addr, uint64 addrlen_ptr)
{
    proc_t *p = myproc();
    struct file *f;
    struct socket *sock;

    if (sockfd < 0 || sockfd >= NOFILE || (f = p->ofile[sockfd]) == 0)
        return -EBADF;
    if (f->f_flags & O_PATH)
        return -EBADF;
    if (f->f_type != FD_SOCKET)
        return -ENOTSOCK;

    sock = f->f_data.sock;
    if (sock->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (sock->state != SOCKET_LISTENING)
        return -EINVAL;

    if (addrlen_ptr)
    {
        uint32 user_addrlen = 0;
        uint32 actual_len = sizeof(struct sockaddr_in);

        if (copyin(p->pagetable, (char *)&user_addrlen, addrlen_ptr, sizeof(user_addrlen)) < 0)
            return -EFAULT;
        if (addr && user_addrlen >= actual_len &&
            copyout(p->pagetable, addr, (char *)&sock->remote_addr, actual_len) < 0)
            return -EFAULT;
        if (copyout(p->pagetable, addrlen_ptr, (char *)&actual_len, sizeof(actual_len)) < 0)
            return -EFAULT;
    }

    if (sock->pending_conn)
    {
        int newfd = alloc_connected_socket_fd(sock, &sock->pending_remote_addr);

        if (newfd < 0)
            return newfd;
        sock->pending_conn = 0;
        return newfd;
    }

    return -EAGAIN;
}

/**
 * @brief 获取套接字绑定地址
 *
 * @param sockfd 套接字描述符
 * @param addr 地址结构指针
 * @param addrlen 地址结构长度指针
 * @return int 状态码
 */
int sys_getsockname(int sockfd, uint64 addr, uint64 addrlen_ptr)
{
    DEBUG_LOG_LEVEL(LOG_INFO, "[sys_getsockname] sockfd: %d, addr: %p, addrlen: %p\n",
                    sockfd, addr, addrlen_ptr);

    struct proc *p = myproc();
    struct file *f;
    uint32 addrlen;
    struct sockaddr_in socket_addr;

    // 获取文件结构体
    if ((f = p->ofile[sockfd]) == 0)
        return -1;

    // 检查是否为套接字
    if (f->f_type != FD_SOCKET)
        return -1;

    // 检查绑定状态
    if (f->f_data.sock->state == SOCKET_UNBOUND)
    {
        DEBUG_LOG_LEVEL(LOG_ERROR, "Socket not bound\n");
        return -1;
    }

    // 从用户空间获取地址长度
    if (copyin(p->pagetable, (char *)&addrlen, addrlen_ptr, sizeof(addrlen)))
    {
        return -1;
    }

    // 准备返回的地址信息
    memmove(&socket_addr, &f->f_data.sock->local_addr, sizeof(struct sockaddr_in));

    // 复制地址到用户空间
    if (copyout(p->pagetable, addr, (char *)&socket_addr, sizeof(socket_addr)))
    {
        return -1;
    }

    // 更新实际使用的地址长度
    addrlen = sizeof(struct sockaddr_in);
    if (copyout(p->pagetable, addrlen_ptr, (char *)&addrlen, sizeof(addrlen)))
    {
        return -1;
    }

    return 0;
}

/**
 * @brief 设置套接字选项
 *
 * @param sockfd 套接字描述符
 * @param level 选项级别（SOL_SOCKET 等）
 * @param optname 选项名称
 * @param optval 选项值指针
 * @param optlen 选项值长度
 * @return int 成功返回0，失败返回-1
 */
int sys_setsockopt(int sockfd, int level, int optname, uint64 optval, uint64 optlen)
{
    DEBUG_LOG_LEVEL(LOG_INFO, "[sys_setsockopt] sockfd: %d, level: %d, optname: %d, optval: %p, optlen: %d\n",
                    sockfd, level, optname, optval, optlen);

    proc_t *p = myproc();
    struct file *f;
    int value;

    // 获取文件描述符对应的文件结构
    if (sockfd < 0 || sockfd >= NOFILE || (f = p->ofile[sockfd]) == 0)
    {
        return -1;
    }

    // 检查文件类型
    if (f->f_type != FD_SOCKET)
    {
        return -1;
    }

    struct socket *sock = f->f_data.sock;
    // 复制用户空间的数据
    if (optlen < sizeof(int))
    {
        return -1;
    }

    if (copyin(p->pagetable, (char *)&value, optval, sizeof(value)) < 0)
    {
        return -1;
    }
    // 处理不同级别的选项
    switch (level)
    {
    case SOL_SOCKET:
        switch (optname)
        {
        case SO_REUSEADDR:
            // sock->reuse_addr = (value != 0);
            DEBUG_LOG_LEVEL(LOG_DEBUG, "Set SO_REUSEADDR: %d\n", value);
            break;

        case SO_KEEPALIVE:
            // sock->keepalive = (value != 0);
            DEBUG_LOG_LEVEL(LOG_DEBUG, "Set SO_KEEPALIVE: %d\n", value);
            break;
        case SO_RCVTIMEO:
        {
            // 验证长度
            if (optlen < sizeof(struct timeval))
                return -EINVAL;

            // 复制时间结构
            struct timeval tv;
            if (copyin(p->pagetable, (char *)&tv, optval, sizeof(tv)) < 0)
            {
                return -EFAULT;
            }

            // 存储到socket结构
            sock->rcv_timeout = tv;
            DEBUG_LOG_LEVEL(LOG_DEBUG, "Set SO_RCVTIMEO: %ld sec, %ld usec\n",
                            tv.sec, tv.usec);
            break;
        }

        default:
            DEBUG_LOG_LEVEL(LOG_WARNING, "Unsupported option: %d\n", optname);
            return -1;
        }
        break;

    default:
        DEBUG_LOG_LEVEL(LOG_WARNING, "Unsupported level: %d\n", level);
        return -1;
    }

    return 0;
}
/**
 * @brief 发送数据到指定的网络地址
 *
 * @param sockfd 套接字描述符
 * @param buf 数据缓冲区指针
 * @param len 数据长度
 * @param flags 发送标志
 * @param addr 目标地址指针
 * @param addrlen 地址长度
 * @return int 发送的字节数或错误码
 */
static struct simple_packet packet_store[MAX_PACKETS] = {0};
int sys_sendto(int sockfd, uint64 buf, int len, int flags, uint64 addr, int addrlen)
{
    DEBUG_LOG_LEVEL(LOG_INFO, "[sys_sendto] sockfd: %d, buf: %p, len: %d, flags: %d, addr: %p, addrlen: %d\n",
                    sockfd, buf, len, flags, addr, addrlen);

    struct proc *p = myproc();
    struct file *f;
    struct sockaddr_in dest_addr;
    char *kbuf;

    // 获取文件结构体
    if ((f = p->ofile[sockfd]) == 0)
        return -1;

    // 检查是否为套接字
    if (f->f_type != FD_SOCKET)
        return -1;

    // 检查套接字状态
    if (f->f_data.sock->state < SOCKET_BOUND)
    {
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = PF_INET;
        local_addr.sin_addr = INADDR_ANY; // 任意本地地址
        int port = 2000;                  // 隐式绑定 2000端口
        local_addr.sin_port = port;       // 任意本地地址
        int bind_result = sock_bind(f->f_data.sock, (struct sockaddr_in *)&local_addr, sizeof(local_addr));
        if (bind_result < 0)
        {
            DEBUG_LOG_LEVEL(LOG_ERROR, "Implicit bind failed: %d\n", bind_result);
            return bind_result;
        }
    }

    // 验证数据长度
    if (len < 0 || len > MAX_PACKET_SIZE)
    {
        return -1;
    }

    // 分配内核缓冲区
    kbuf = kalloc();
    if (!kbuf)
    {
        return -1;
    }

    // 从用户空间复制数据
    if (copyin(p->pagetable, kbuf, buf, len) < 0)
    {
        kfree(kbuf);
        return -1;
    }

    // 处理目标地址
    if (addr)
    {
        if (addrlen < sizeof(struct sockaddr_in))
        {
            DEBUG_LOG_LEVEL(LOG_ERROR, "Address length too small: %d < %d\n",
                            addrlen, sizeof(struct sockaddr_in));
            kfree(kbuf);
            return -EINVAL;
        }
        // 从用户空间复制地址结构
        if (copyin(p->pagetable, (char *)&dest_addr, addr, sizeof(dest_addr)))
        {
            kfree(kbuf);
            return -1;
        }
    }
    else
    {
        // 如果没有提供地址，使用连接的目标地址
        if (f->f_data.sock->state != SOCKET_CONNECTED)
        {
            DEBUG_LOG_LEVEL(LOG_ERROR, "Socket not connected and no address provided\n");
            kfree(kbuf);
            return -1;
        }
        memcpy(&dest_addr, &f->f_data.sock->remote_addr, sizeof(dest_addr));
    }
    ///< 找到空闲缓冲区并写入
    int send_len = -1;
    for (int i = 0; i < MAX_PACKETS; i++)
    {
        if (!packet_store[i].valid)
        {
            packet_store[i].dst_port = dest_addr.sin_port;
            packet_store[i].data = kbuf;
            packet_store[i].valid = 1;
            send_len = strlen(kbuf);
            break;
        }
    }

    // kfree(kbuf);
    return send_len;
}

/**
 * @brief 从套接字接收数据并获取发送方地址
 *
 * @param sockfd 套接字描述符
 * @param buf 数据缓冲区指针
 * @param len 缓冲区长度
 * @param flags 接收标志
 * @param addr 发送方地址指针
 * @param addrlen 地址长度指针
 * @return int 接收的字节数或错误码
 */
int sys_recvfrom(int sockfd, uint64 buf, int len, int flags, uint64 addr, uint64 addrlen_ptr)
{
    DEBUG_LOG_LEVEL(LOG_INFO, "[sys_recvfrom] sockfd: %d, buf: %p, len: %d, flags: %d, addr: %p, addrlen: %p\n",
                    sockfd, buf, len, flags, addr, addrlen_ptr);

    struct proc *p = myproc();
    struct file *f;
    char *kbuf;

    // 获取文件结构体
    if ((f = p->ofile[sockfd]) == 0)
        return -1;

    // 检查是否为套接字
    if (f->f_type != FD_SOCKET)
        return -1;

    // 检查套接字状态
    if (f->f_data.sock->state < SOCKET_BOUND)
    {
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = PF_INET;
        local_addr.sin_addr = INADDR_ANY;
        local_addr.sin_port = 2000; // 默认端口
        int bind_result = sock_bind(f->f_data.sock, &local_addr, sizeof(local_addr));
        if (bind_result < 0)
            return bind_result;
    }

    // 验证缓冲区长度
    if (len < 0 || len > MAX_PACKET_SIZE)
    {
        return -1;
    }

    // 分配内核缓冲区
    kbuf = kalloc();
    if (!kbuf)
    {
        return -1;
    }

    uint16_t local_port = f->f_data.sock->local_addr.sin_port;
    char *recv_byte = 0;
    int found = 0;
    int recv_len = -1;

    for (int i = 0; i < MAX_PACKETS; i++)
    {
        if (packet_store[i].valid && packet_store[i].dst_port == local_port)
        {
            recv_byte = packet_store[i].data;
            packet_store[i].valid = 0; // 标记为无效
            found = 1;
            recv_len = strlen(recv_byte);
            break;
        }
    }
    if (!found)
        return -1; // 没有找到数据包

    // 将数据复制到用户空间
    if (copyout(p->pagetable, buf, recv_byte, 1) < 0)
    {
        return -1;
    }

    // 处理发送方地址
    // 返回源地址信息（固定为127.0.0.1）
    if (addr && addrlen_ptr)
    {
        struct sockaddr_in src_addr;
        memset(&src_addr, 0, sizeof(src_addr));
        src_addr.sin_family = PF_INET;
        src_addr.sin_addr = 0x7f000001; // 127.0.0.1
        src_addr.sin_port = 2000;       // 固定源端口

        // 复制地址到用户空间
        if (copyout(p->pagetable, addr, (char *)&src_addr, sizeof(src_addr)) < 0)
        {
            return -1;
        }

        // 更新地址长度
        int addrlen_val = sizeof(src_addr);
        if (copyout(p->pagetable, addrlen_ptr, (char *)&addrlen_val, sizeof(addrlen_val)))
        {
            return -1;
        }
    }

    kfree(kbuf);
    return recv_len;
}
