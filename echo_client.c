#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <liburing.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 12345
#define QUEUE_DEPTH 16
#define BUF_SIZE 1024

enum {
    EVENT_CONNECT,
    EVENT_SEND,
    EVENT_RECV
};

struct io_event {
    int event;
    int fd;
    char buf[BUF_SIZE];
};

static void add_connect(struct io_uring *ring, int fd);
static void add_send(struct io_uring *ring, int fd, char *buf, int len);
static void add_recv(struct io_uring *ring, int fd);

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct io_uring ring;
    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);

    add_connect(&ring, fd);

    while (1) {
        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&ring, &cqe);
        struct io_event *ev = (struct io_event *)cqe->user_data;

        if (ev->event == EVENT_CONNECT) {
            printf("连接成功！\n");
            free(ev);

            // 发送测试消息
            char msg[] = "Hello io_uring echo!";
            add_send(&ring, fd, msg, strlen(msg));
        }
        else if (ev->event == EVENT_SEND) {
            printf("发送: %s\n", ev->buf);
            free(ev);
            add_recv(&ring, fd);
        }
        else if (ev->event == EVENT_RECV) {
            int n = cqe->res;
            if (n <= 0) {
                printf("服务端断开\n");
                close(fd);
                free(ev);
                break;
            }
            printf("回显: %.*s\n", n, ev->buf);
            free(ev);

            // 继续从键盘输入
            char input[BUF_SIZE];
            printf("输入消息: ");
            fgets(input, BUF_SIZE, stdin);
            input[strcspn(input, "\n")] = 0;
            add_send(&ring, fd, input, strlen(input));
        }

        io_uring_cqe_seen(&ring, cqe);
    }

    io_uring_queue_exit(&ring);
    return 0;
}

static void add_connect(struct io_uring *ring, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct io_event *ev = calloc(1, sizeof(*ev));
    ev->event = EVENT_CONNECT;
    ev->fd = fd;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(SERVER_PORT),
        .sin_addr.s_addr = inet_addr(SERVER_IP)
    };

    io_uring_prep_connect(sqe, fd, (struct sockaddr *)&addr, sizeof(addr));
    io_uring_sqe_set_data(sqe, ev);
    io_uring_submit(ring);
}

static void add_send(struct io_uring *ring, int fd, char *buf, int len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct io_event *ev = malloc(sizeof(*ev));
    ev->event = EVENT_SEND;
    ev->fd = fd;
    memcpy(ev->buf, buf, len);
    io_uring_prep_send(sqe, fd, ev->buf, len, 0);
    io_uring_sqe_set_data(sqe, ev);
    io_uring_submit(ring);
}

static void add_recv(struct io_uring *ring, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct io_event *ev = malloc(sizeof(*ev));
    ev->event = EVENT_RECV;
    ev->fd = fd;
    io_uring_prep_recv(sqe, fd, ev->buf, BUF_SIZE, 0);
    io_uring_sqe_set_data(sqe, ev);
    io_uring_submit(ring);
}
