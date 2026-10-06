/*
 * The libgit2 workload of the corpus gate's overhead benchmark (RFC 0034,
 * stage S0): a history built and read back in memory, with no file system
 * or network.
 *
 *   libgit2-bench [commits]      (default 1500)
 *
 * A repository wraps an object database with only the in-memory mempack
 * backend and an empty configuration (so no user or system configuration
 * is read). Eight generated text files of 400 lines evolve over the
 * commits: each commit edits two of them (replaced, inserted and deleted
 * lines), writes their blobs (SHA-1 with collision detection), a tree and a
 * commit, renders the patch of each edit with git_patch_from_buffers
 * (xdiff), and merges each edit three-way with an unrelated edit of the
 * same file (git_merge_file, xmerge). Then a revision walk visits the whole
 * history and diffs every commit's tree against its parent's, with stats.
 * Everything is generated from a fixed seed with fixed signature times; the
 * program prints one checksum line over the object ids, patches, merge
 * results and stats, which the gate compares between the reference and the
 * WeaveC build. The gate builds it against the static libgit2 of the
 * pinned checkout (see test/corpus/manifest.json, config libgit2).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <git2.h>
#include <git2/sys/mempack.h>
#include <git2/sys/repository.h>

#define FILES 8
#define LINES 400
#define LINE_SIZE 72

struct document {
  char **lines;
  size_t count;
  size_t capacity;
};

static unsigned long nextRandom(unsigned long *state) {
  *state = *state * 1103515245UL + 12345UL;
  return (*state >> 8) & 0xffffffUL;
}

static unsigned long long checksum = 1469598103934665603ULL;

static void mixBytes(const char *data, size_t size) {
  size_t i;
  for(i = 0; i < size; i++)
    checksum = (checksum ^ (unsigned char)data[i]) * 1099511628211ULL;
  checksum = (checksum ^ 0u) * 1099511628211ULL;
}

static void mixNumber(long long value) {
  char text[32];
  snprintf(text, sizeof(text), "%lld", value);
  mixBytes(text, strlen(text));
}

static void mixOid(const git_oid *oid) {
  char text[GIT_OID_MAX_HEXSIZE + 1];
  git_oid_tostr(text, sizeof(text), oid);
  mixBytes(text, strlen(text));
}

static void fail(const char *what) {
  const git_error *error = git_error_last();
  fprintf(stderr, "libgit2-bench: %s: %s\n", what,
          error && error->message ? error->message : "failed");
  exit(1);
}

static char *makeLine(unsigned long *state) {
  static const char *const words[] = {
      "alpha", "beta",  "gamma", "delta", "object", "tree", "blob",
      "index", "merge", "patch", "hunk",  "commit", "ref",  "pack"};
  char *line = malloc(LINE_SIZE);
  int length = 0;
  if(!line)
    fail("malloc");
  length = snprintf(line, LINE_SIZE, "%06lu", nextRandom(state) % 1000000);
  while(length < LINE_SIZE - 12) {
    const char *word = words[nextRandom(state) % 14];
    length += snprintf(line + length, (size_t)(LINE_SIZE - length), " %s", word);
  }
  return line;
}

static void insertLine(struct document *doc, size_t at, char *line) {
  if(doc->count == doc->capacity) {
    size_t capacity = doc->capacity ? doc->capacity * 2 : 64;
    char **lines = realloc(doc->lines, capacity * sizeof(char *));
    if(!lines)
      fail("realloc");
    doc->lines = lines;
    doc->capacity = capacity;
  }
  memmove(doc->lines + at + 1, doc->lines + at,
          (doc->count - at) * sizeof(char *));
  doc->lines[at] = line;
  doc->count++;
}

static void deleteLine(struct document *doc, size_t at) {
  free(doc->lines[at]);
  memmove(doc->lines + at, doc->lines + at + 1,
          (doc->count - at - 1) * sizeof(char *));
  doc->count--;
}

static void editDocument(struct document *doc, int edits, unsigned long *state) {
  int i;
  for(i = 0; i < edits; i++) {
    unsigned long kind = nextRandom(state) % 3;
    size_t at = doc->count ? nextRandom(state) % doc->count : 0;
    if(kind == 0 && doc->count) {
      free(doc->lines[at]);
      doc->lines[at] = makeLine(state);
    } else if(kind == 1 || doc->count < LINES / 2) {
      insertLine(doc, at, makeLine(state));
    } else {
      deleteLine(doc, at);
    }
  }
}

static char *render(const struct document *doc, size_t *size) {
  size_t total = 0;
  size_t i;
  char *text;
  char *out;
  for(i = 0; i < doc->count; i++)
    total += strlen(doc->lines[i]) + 1;
  text = malloc(total + 1);
  if(!text)
    fail("malloc");
  out = text;
  for(i = 0; i < doc->count; i++) {
    size_t length = strlen(doc->lines[i]);
    memcpy(out, doc->lines[i], length);
    out[length] = '\n';
    out += length + 1;
  }
  *out = '\0';
  *size = total;
  return text;
}

static void patchAndMerge(const char *before, size_t beforeSize,
                          const char *after, size_t afterSize,
                          const char *theirs, size_t theirsSize,
                          const char *path) {
  git_patch *patch = NULL;
  git_buf text = GIT_BUF_INIT;
  git_merge_file_input ancestor, ours, other;
  git_merge_file_result result;

  if(git_patch_from_buffers(&patch, before, beforeSize, path, after, afterSize,
                            path, NULL) < 0 ||
     git_patch_to_buf(&text, patch) < 0)
    fail("patch");
  mixBytes(text.ptr, text.size);
  git_buf_dispose(&text);
  git_patch_free(patch);

  git_merge_file_input_init(&ancestor, GIT_MERGE_FILE_INPUT_VERSION);
  git_merge_file_input_init(&ours, GIT_MERGE_FILE_INPUT_VERSION);
  git_merge_file_input_init(&other, GIT_MERGE_FILE_INPUT_VERSION);
  ancestor.ptr = before;
  ancestor.size = beforeSize;
  ancestor.path = path;
  ours.ptr = after;
  ours.size = afterSize;
  ours.path = path;
  other.ptr = theirs;
  other.size = theirsSize;
  other.path = path;
  memset(&result, 0, sizeof(result));
  if(git_merge_file(&result, &ancestor, &ours, &other, NULL) < 0)
    fail("merge_file");
  mixNumber(result.automergeable);
  mixBytes(result.ptr, result.len);
  git_merge_file_result_free(&result);
}

static void walkHistory(git_repository *repo, const git_oid *head) {
  git_revwalk *walk = NULL;
  git_oid id;
  long commits = 0;

  if(git_revwalk_new(&walk, repo) < 0 ||
     git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME) < 0 ||
     git_revwalk_push(walk, head) < 0)
    fail("revwalk");
  while(git_revwalk_next(&id, walk) == 0) {
    git_commit *commit = NULL;
    git_commit *parent = NULL;
    git_tree *tree = NULL;
    git_tree *parentTree = NULL;
    git_diff *diff = NULL;
    git_diff_stats *stats = NULL;

    if(git_commit_lookup(&commit, repo, &id) < 0 ||
       git_commit_tree(&tree, commit) < 0)
      fail("commit");
    mixBytes(git_commit_message(commit), strlen(git_commit_message(commit)));
    mixNumber((long long)git_commit_time(commit));
    if(git_commit_parentcount(commit) > 0) {
      if(git_commit_parent(&parent, commit, 0) < 0 ||
         git_commit_tree(&parentTree, parent) < 0)
        fail("parent");
    }
    if(git_diff_tree_to_tree(&diff, repo, parentTree, tree, NULL) < 0 ||
       git_diff_get_stats(&stats, diff) < 0)
      fail("diff");
    mixNumber((long long)git_diff_stats_files_changed(stats));
    mixNumber((long long)git_diff_stats_insertions(stats));
    mixNumber((long long)git_diff_stats_deletions(stats));
    git_diff_stats_free(stats);
    git_diff_free(diff);
    git_tree_free(parentTree);
    git_tree_free(tree);
    git_commit_free(parent);
    git_commit_free(commit);
    commits++;
  }
  mixNumber(commits);
  git_revwalk_free(walk);
}

int main(int argc, char **argv) {
  int commits = argc > 1 ? atoi(argv[1]) : 1500;
  unsigned long state = 1;
  struct document docs[FILES];
  git_oid blobs[FILES];
  git_oid head;
  git_odb *odb = NULL;
  git_odb_backend *mempack = NULL;
  git_repository *repo = NULL;
  git_config *config = NULL;
  int hasHead = 0;
  int n;
  int f;

  if(git_libgit2_init() < 0)
    fail("init");
  if(git_odb_new(&odb) < 0 || git_mempack_new(&mempack) < 0 ||
     git_odb_add_backend(odb, mempack, 1) < 0 ||
     git_repository_wrap_odb(&repo, odb) < 0 || git_config_new(&config) < 0 ||
     git_repository_set_config(repo, config) < 0)
    fail("repository");

  memset(docs, 0, sizeof(docs));
  for(f = 0; f < FILES; f++) {
    int i;
    for(i = 0; i < LINES; i++)
      insertLine(&docs[f], docs[f].count, makeLine(&state));
  }
  for(f = 0; f < FILES; f++) {
    size_t size;
    char *text = render(&docs[f], &size);
    if(git_blob_create_from_buffer(&blobs[f], repo, text, size) < 0)
      fail("blob");
    free(text);
  }

  for(n = 0; n < commits; n++) {
    git_treebuilder *builder = NULL;
    git_signature *signature = NULL;
    git_tree *tree = NULL;
    git_commit *parent = NULL;
    const git_commit *parents[1];
    git_oid treeId;
    char message[96];
    int edit;

    for(edit = 0; edit < 2; edit++) {
      char path[32];
      size_t beforeSize, afterSize, theirsSize;
      char *before, *after, *theirs;
      struct document other;
      size_t i;

      f = (int)(nextRandom(&state) % FILES);
      snprintf(path, sizeof(path), "src/file%d.txt", f);
      before = render(&docs[f], &beforeSize);
      /* An unrelated edit: the last few lines replaced. */
      memset(&other, 0, sizeof(other));
      for(i = 0; i < docs[f].count; i++) {
        char *copy = malloc(LINE_SIZE);
        if(!copy)
          fail("malloc");
        if(i + 3 < docs[f].count)
          memcpy(copy, docs[f].lines[i], strlen(docs[f].lines[i]) + 1);
        else
          snprintf(copy, LINE_SIZE, "tail %d %lu", n, nextRandom(&state));
        insertLine(&other, other.count, copy);
      }
      theirs = render(&other, &theirsSize);
      while(other.count)
        deleteLine(&other, other.count - 1);
      free(other.lines);

      editDocument(&docs[f], 6, &state);
      after = render(&docs[f], &afterSize);
      if(git_blob_create_from_buffer(&blobs[f], repo, after, afterSize) < 0)
        fail("blob");
      mixOid(&blobs[f]);
      patchAndMerge(before, beforeSize, after, afterSize, theirs, theirsSize,
                    path);
      free(before);
      free(after);
      free(theirs);
    }

    if(git_treebuilder_new(&builder, repo, NULL) < 0)
      fail("treebuilder");
    for(f = 0; f < FILES; f++) {
      char name[32];
      snprintf(name, sizeof(name), "file%d.txt", f);
      if(git_treebuilder_insert(NULL, builder, name, &blobs[f],
                                GIT_FILEMODE_BLOB) < 0)
        fail("treebuilder_insert");
    }
    if(git_treebuilder_write(&treeId, builder) < 0 ||
       git_tree_lookup(&tree, repo, &treeId) < 0)
      fail("tree");
    git_treebuilder_free(builder);

    if(git_signature_new(&signature, "WeaveC Bench", "bench@example.org",
                         (git_time_t)1700000000 + (git_time_t)n * 60, 0) < 0)
      fail("signature");
    if(hasHead && git_commit_lookup(&parent, repo, &head) < 0)
      fail("parent");
    parents[0] = parent;
    snprintf(message, sizeof(message), "Commit %d\n\nTwo files edited.\n", n);
    if(git_commit_create(&head, repo, NULL, signature, signature, NULL, message,
                         tree, hasHead ? 1 : 0, parents) < 0)
      fail("commit");
    mixOid(&head);
    hasHead = 1;
    git_commit_free(parent);
    git_signature_free(signature);
    git_tree_free(tree);
  }

  if(hasHead)
    walkHistory(repo, &head);

  for(f = 0; f < FILES; f++) {
    while(docs[f].count)
      deleteLine(&docs[f], docs[f].count - 1);
    free(docs[f].lines);
  }
  git_config_free(config);
  git_repository_free(repo);
  git_odb_free(odb);
  git_libgit2_shutdown();
  printf("libgit2-bench %d commits checksum %016llx\n", commits, checksum);
  return 0;
}
