// The callee of const-shallow-local.c, in its own unit.
struct out { int *count; const char **name; };
struct request { struct out *out; int id; };

int fill(const struct request *req) {
  *req->out->count = req->id;
  *req->out->name = "seven";
  return 0;
}
