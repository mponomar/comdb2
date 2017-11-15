#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <time.h>
#include <tcputil.h>

#include <unistd.h>
#include <poll.h>
#include <sys/time.h>

struct ready_notifier {
    int pipefd[2];
    struct pollfd *fds;
    int numfds;
    int allocedfds;
    int64_t *starttime;
    void *usrptr;
    void (*callback)(void *, int);
    int maxwaitms;
    pthread_t handler_thread;
};

static void delfd(struct ready_notifier *n, int slot)
{
    if (slot == 0) {
        fprintf(stderr, "%s: unexpected: can't delete last slot\n", __func__);
        return;
    }
    n->numfds--;
    if (slot != n->numfds) {
        n->fds[slot] = n->fds[n->numfds];
        n->starttime[slot] = n->starttime[n->numfds];
    }
    n->fds[n->numfds].fd = -1;
    n->fds[n->numfds].events = 0;
    n->fds[n->numfds].revents = 0;
}

static int64_t gettime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

static void *ready_notifier_thread(void *p)
{
    struct ready_notifier *n;
    int fd;

    n = (struct ready_notifier *)p;

    for (;;) {
        int rc;
        rc = poll(n->fds, n->numfds, n->maxwaitms);

        int64_t now = gettime();
        n->starttime[0] = now;

#if 0
        printf("poll rc %d  numfds %d alloc %d\n", rc, n->numfds, n->allocedfds);
        for (int i = 0; i < n->numfds; i++) {
            printf("%d  fd %d in %x out %x age %dms\n", i, n->fds[i].fd, n->fds[i].events, n->fds[i].revents,
                    (now - n->starttime[i]) / 1000);
        }
        printf("\n");
#endif
        for (int i = 0; i < n->numfds; i++) {
            n->fds[i].events = POLLIN | POLLERR;

            if (n->fds[i].revents & POLLIN) {
                if (i == 0) {
                    /* we have a new connection */
                    rc = read(n->pipefd[0], &fd, sizeof(int));
                    if (rc != sizeof(int)) {
                        fprintf(stderr, "%s: unexpected read from pipe: rc %d "
                                        "errno %d %s\n",
                                __func__, rc, errno, strerror(errno));
                        continue;
                    }

                    // printf("before: got new connection to monitor %d\n", fd);
                    /* this is how we're signalled to exit */
                    if (fd == -1) goto done;

                    fprintf(stderr, "numfds %d allocedfds %d\n", n->numfds, n->allocedfds);

                    if (n->numfds >= n->allocedfds) {
                        struct pollfd *fds;
                        fprintf(stderr, "realloc\n");
                        fds = realloc(n->fds,
                                      (n->allocedfds * 2 + 10) *
                                          sizeof(struct pollfd));
                        if (fds == NULL) {
                            fprintf(stderr, "%s: can't allocate memory for %d "
                                            "file descriptors\n",
                                    __func__, n->allocedfds * 2 + 10);
                            rc = close(fd);
                            if (rc)
                                fprintf(stderr, "%s:%d close(%d) rc %d %s\n",
                                        __func__, __LINE__, fd, errno,
                                        strerror(errno));
                            delfd(n, i--);
                            continue;
                        }
                        int64_t *starttime =
                            realloc(n->starttime,
                                    (n->allocedfds * 2 + 10) * sizeof(int64_t));
                        if (starttime == NULL) {
                            fprintf(
                                stderr,
                                "%s: can't allocate memory for %d wait times\n",
                                __func__, n->allocedfds * 2 + 10);
                            rc = close(fd);
                            if (rc)
                                fprintf(stderr, "%s:%d close(%d) rc %d %s\n",
                                        __func__, __LINE__, fd, errno,
                                        strerror(errno));
                            delfd(n, i--);

                            continue;
                        }
                        n->fds = fds;
                        n->starttime = starttime;
                        n->allocedfds = n->allocedfds * 2 + 10;
                    }
                    n->fds[n->numfds].fd = fd;
                    n->fds[n->numfds].events = POLLIN | POLLERR;
                    n->fds[n->numfds].revents = 0;
                    n->starttime[n->numfds] = gettime();
                    n->numfds++;
                    // printf("after: got new connection to monitor %d\n", fd);
                } else {
                    fd = n->fds[i].fd;
                    delfd(n, i--);
                    n->callback(n->usrptr, fd);
                }
            } else if (n->fds[i].revents & POLLERR) {
                fd = n->fds[i].fd;
                rc = close(fd);
                if (rc) {
                    /* uh oh */
                    fprintf(stderr, "%s:%d close(%d) rc %d %s\n", __func__,
                            __LINE__, fd, errno, strerror(errno));
                }
                delfd(n, i--);
            } else {
                /* see how long this has been idle */
                if (i != 0) {
                    if ((now - n->starttime[i]) / 1000 > n->maxwaitms) {
                        // printf("timing out conn fd %d\n", n->fds[i].fd);
                        rc = close(n->fds[i].fd);
                        if (rc)
                            fprintf(stderr, "%s:%d close(%d) rc %d %s\n",
                                    __func__, __LINE__, fd, errno,
                                    strerror(errno));
                        delfd(n, i--);
                    }
                }
            }
        }
    }
done:
    return NULL;
}

static int initial_notify_fds = 20;

struct ready_notifier *ready_notifier_create(void *usrptr,
                                             void (*callback)(void *, int),
                                             int maxwaitms)
{
    struct ready_notifier *n;
    int rc;

    n = calloc(1, sizeof(struct ready_notifier));
    if (n == NULL) goto err;
    rc = pipe(n->pipefd);
    if (rc) goto err;
    n->fds = malloc(initial_notify_fds * sizeof(struct pollfd));
    if (n->fds == NULL) goto err;
    n->allocedfds = initial_notify_fds;
    n->numfds = 1;
    n->usrptr = usrptr;
    n->callback = callback;
    n->fds[0].fd = n->pipefd[0];
    n->fds[0].events = POLLIN;
    n->maxwaitms = maxwaitms;
    n->starttime = malloc(initial_notify_fds * sizeof(int64_t));
    if (n->starttime == NULL) goto err;
    rc = pthread_create(&n->handler_thread, NULL, ready_notifier_thread, n);
    if (rc) goto err;

    return n;
err:
    /* TODO: free partially created notifier */
    return NULL;
}

int ready_notifier_add(struct ready_notifier *n, int fd)
{
    int rc;
    rc = write(n->pipefd[1], &fd, sizeof(int));
    if (rc != sizeof(int)) return -1;
    return 0;
}

void ready_notifier_shutdown(struct ready_notifier *n)
{
    int fd = -1;
    void *retval;

    write(n->pipefd[1], &fd, sizeof(int));
    pthread_join(n->handler_thread, &retval);

    close(n->pipefd[0]);
    close(n->pipefd[1]);
    for (int i = 0; i < n->numfds; i++)
        close(n->fds[i].fd);
    free(n->fds);
    free(n->starttime);
}
