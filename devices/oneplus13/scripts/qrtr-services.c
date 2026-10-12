// SPDX-License-Identifier: GPL-2.0-only
/* Bounded, read-only service discovery; no service payloads or modem commands. */
#include <endian.h>
#include <errno.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("QRTR socket"); return 1; }
    struct sockaddr_qrtr local = {.sq_family = AF_QIPCRTR};
    socklen_t len = sizeof(local);
    if (getsockname(fd, (void *)&local, &len)) return 1;
    if (bind(fd, (void *)&local, sizeof(local))) { perror("bind"); return 1; }
    struct sockaddr_qrtr ctrl = {
        .sq_family = AF_QIPCRTR, .sq_node = local.sq_node, .sq_port = QRTR_PORT_CTRL
    };
    struct qrtr_ctrl_pkt request = {.cmd = htole32(QRTR_TYPE_NEW_LOOKUP)};
    if (sendto(fd, &request, sizeof(request), 0, (void *)&ctrl, sizeof(ctrl)) != sizeof(request)) {
        perror("lookup"); return 1;
    }
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    puts("service version instance node port");
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 5) break;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 500) <= 0) continue;
        struct qrtr_ctrl_pkt response;
        struct sockaddr_qrtr from;
        len = sizeof(from);
        ssize_t n = recvfrom(fd, &response, sizeof(response), 0, (void *)&from, &len);
        if (n != sizeof(response) || from.sq_node != ctrl.sq_node ||
            from.sq_port != QRTR_PORT_CTRL || le32toh(response.cmd) != QRTR_TYPE_NEW_SERVER)
            continue;
        unsigned service = le32toh(response.server.service);
        unsigned instance = le32toh(response.server.instance);
        if (!service) break;
        printf("%u %u %u %u %u\n", service, instance & 255, instance >> 8,
               le32toh(response.server.node), le32toh(response.server.port));
    }
    close(fd);
    return 0;
}
