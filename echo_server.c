#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <liburing.h>
#include <sys/mman.h>

#define PORT            12345
#define MAX_CONN        (1 << 10)    // 100万连接
#define QUEUE_DEPTH     4096
#define BUF_SIZE        4096

enum {
    CONN_ACTIVE,
    CONN_CLOSED
};

struct connection {
    int     fd;
    int     state;
    int     read_bytes;
    char    buffer[BUF_SIZE];
};

// 重要：不再用静态全局数组，改用动态分配
static struct connection *conn_pool = NULL;
static struct connection **fd_to_conn = NULL;

static struct io_uring ring;

static inline struct connection *acquire_conn(int fd)
{
    if (fd < 0 || fd >= MAX_CONN) return NULL;
    struct connection *c = &conn_pool[fd];
    c->fd = fd;
    c->state = CONN_ACTIVE;
    c->read_bytes = 0;
    fd_to_conn[fd] = c;
    return c;
}

static inline void release_conn(int fd)
{
    if (fd < 0 || fd >= MAX_CONN) return;
    fd_to_conn[fd] = NULL;
    close(fd);
}

static void submit_accept(int listen_fd)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, (void *)(intptr_t)listen_fd);
    io_uring_submit(&ring);
}

static void submit_recv(struct connection *c)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    io_uring_prep_recv(sqe, c->fd, c->buffer, BUF_SIZE, 0);
    io_uring_sqe_set_data(sqe, (void *)(intptr_t)c->fd);
    io_uring_submit(&ring);
}

static void submit_echo(struct connection *c, int len)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) return;
    io_uring_prep_send(sqe, c->fd, c->buffer, len, 0);
    io_uring_sqe_set_data(sqe, (void *)(intptr_t)c->fd);
    io_uring_submit(&ring);
}

static int create_listen_socket()
{
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(listen_fd, 4096);
    return listen_fd;
}

// 初始化连接池（修复核心）
static void init_connection_pool()
{
    conn_pool = mmap(
        NULL,
        MAX_CONN * sizeof(struct connection),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );

    fd_to_conn = mmap(
        NULL,
        MAX_CONN * sizeof(struct connection*),
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1, 0
    );
}

int main()
{
    init_connection_pool(); // 先初始化大内存

    int listen_fd = create_listen_socket();
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

    submit_accept(listen_fd);

    while (1) {
        struct io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) continue;

        int fd = (intptr_t)cqe->user_data;
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        if (fd == listen_fd) {
            if (res >= 0) {
                acquire_conn(res);
                submit_recv(fd_to_conn[res]);
            }
            submit_accept(listen_fd);
            continue;
        }

        struct connection *conn = fd_to_conn[fd];
        if (!conn || res < 0) {
            release_conn(fd);
            continue;
        }

        if (res == 0) {
            release_conn(fd);
            continue;
        }

        if (res > 0) {
            submit_echo(conn, res);
            submit_recv(conn);
        }
    }

    io_uring_queue_exit(&ring);
    close(listen_fd);
    return 0;
}
