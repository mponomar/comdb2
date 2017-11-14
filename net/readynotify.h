#ifndef INCLUDED_READYNOTIFY_H
#define INCLUDED_READYNOTIFY_H

struct ready_notifier;

struct ready_notifier *ready_notifier_create(void *usrptr, void (*callback)(void*, int), int maxwaitms);
int ready_notifier_add(struct ready_notifier *n, int fd);
void ready_notifier_shutdown(struct ready_notifier *n);

#endif
