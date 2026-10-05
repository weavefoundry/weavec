#ifndef MSG_H
#define MSG_H
struct msg {
  char *body;
  int prio;
};
struct msg *msg_new(const char *body, int prio);
/* sends the message; takes ownership of it */
int msg_send(struct msg *m);
#endif
