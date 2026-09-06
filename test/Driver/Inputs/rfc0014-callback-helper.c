// RFC 0014: the helper body must be specialized at link time.
void invoke(void (*callback)(void *), void *userdata) {
  callback(userdata);
}
