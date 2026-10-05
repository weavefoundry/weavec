/*
 * The curl workload of the corpus gate's overhead benchmark (RFC 0034,
 * stage S0): libcurl's own parsers without any transfer, so no socket,
 * file or kernel time.
 *
 *   curl-bench [rounds]      (default 10)
 *
 * Each round parses 20,000 generated URLs with the URL API (curl_url_set,
 * every part read back URL-decoded, a relative reference resolved against
 * it, a query appended), escapes and unescapes a path, parses a date with
 * curl_getdate for each, and feeds 5,000 cookie-file lines to one easy handle's
 * cookie engine (CURLOPT_COOKIELIST), whose cookie list is read back and
 * cleared at the end of the round. Everything is generated from a fixed
 * seed; the program prints one checksum line over every string libcurl
 * returned, which the gate compares between the reference and the WeaveC
 * build. The gate builds it against the static libcurl of the pinned
 * checkout (see test/corpus/manifest.json, config curl).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#define URLS_PER_ROUND 20000
#define COOKIES_PER_ROUND 5000

static unsigned long nextRandom(unsigned long *state) {
  *state = *state * 1103515245UL + 12345UL;
  return (*state >> 8) & 0xffffffUL;
}

static unsigned long long checksum = 1469598103934665603ULL;

static void mix(const char *text) {
  if(!text) {
    checksum = (checksum ^ 0xffu) * 1099511628211ULL;
    return;
  }
  while(*text) {
    checksum = (checksum ^ (unsigned char)*text++) * 1099511628211ULL;
  }
  checksum = (checksum ^ 0u) * 1099511628211ULL;
}

static void mixNumber(long long value) {
  char text[32];
  snprintf(text, sizeof(text), "%lld", value);
  mix(text);
}

static const char *const schemes[] = {"http", "https", "ftp", "ws",
                                      "imap", "smtp", "gopher", "rtsp"};
static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
                                   "Sun"};

static void makeHost(char *host, size_t size, unsigned long *state) {
  unsigned long kind = nextRandom(state) % 4;
  if(kind == 0)
    snprintf(host, size, "10.%lu.%lu.%lu", nextRandom(state) % 256,
             nextRandom(state) % 256, nextRandom(state) % 256);
  else if(kind == 1)
    snprintf(host, size, "[2001:db8:%lx::%lx]", nextRandom(state) % 65536,
             nextRandom(state) % 65536);
  else
    snprintf(host, size, "Host%lu.Sub-%lu.example%lu.ORG", nextRandom(state) % 5000,
             nextRandom(state) % 40, nextRandom(state) % 30);
}

static void parseUrl(CURLU *url, unsigned long *state) {
  static const CURLUPart parts[] = {
      CURLUPART_SCHEME, CURLUPART_USER,  CURLUPART_PASSWORD,
      CURLUPART_HOST,   CURLUPART_PORT,  CURLUPART_PATH,
      CURLUPART_QUERY,  CURLUPART_FRAGMENT, CURLUPART_ZONEID};
  char host[64];
  char text[512];
  char relative[128];
  char *value;
  size_t i;
  CURLUcode rc;

  makeHost(host, sizeof(host), state);
  snprintf(text, sizeof(text),
           "%s://user%lu:p%%40ss%%3Aw%lu@%s:%lu/a/./b%lu/../c%lu/%%7euser/"
           "d%%20e/f.html?q=%lu&name=a%%2Bb&lang=en#frag%lu",
           schemes[nextRandom(state) % 8], nextRandom(state) % 1000,
           nextRandom(state) % 1000, host, 1 + nextRandom(state) % 65535,
           nextRandom(state) % 100, nextRandom(state) % 100,
           nextRandom(state), nextRandom(state) % 50);
  rc = curl_url_set(url, CURLUPART_URL, text, CURLU_NON_SUPPORT_SCHEME);
  mixNumber(rc);
  if(rc)
    return;
  for(i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
    value = NULL;
    mixNumber(curl_url_get(url, parts[i], &value, CURLU_URLDECODE));
    mix(value);
    curl_free(value);
  }
  value = NULL;
  mixNumber(curl_url_get(url, CURLUPART_URL, &value, CURLU_PUNY2IDN));
  mix(value);
  curl_free(value);
  snprintf(relative, sizeof(relative), "../g%lu/./h i%lu?x=%lu#y",
           nextRandom(state) % 100, nextRandom(state) % 100,
           nextRandom(state) % 1000);
  mixNumber(curl_url_set(url, CURLUPART_URL, relative, CURLU_URLENCODE));
  value = NULL;
  mixNumber(curl_url_get(url, CURLUPART_URL, &value, 0));
  mix(value);
  curl_free(value);
  mixNumber(curl_url_set(url, CURLUPART_QUERY, "added=yes please",
                         CURLU_APPENDQUERY | CURLU_URLENCODE));
  value = NULL;
  mixNumber(curl_url_get(url, CURLUPART_QUERY, &value, 0));
  mix(value);
  curl_free(value);
}

static void escapeText(CURL *easy, unsigned long *state) {
  char text[128];
  char *escaped;
  char *unescaped;
  int length = 0;

  snprintf(text, sizeof(text), "/path %lu/with spaces & symbols=%lu?#[]@!$'()*+,;%%%lu",
           nextRandom(state), nextRandom(state) % 100, nextRandom(state) % 10);
  escaped = curl_easy_escape(easy, text, 0);
  mix(escaped);
  unescaped = escaped ? curl_easy_unescape(easy, escaped, 0, &length) : NULL;
  mix(unescaped);
  mixNumber(length);
  curl_free(unescaped);
  curl_free(escaped);
}

static void parseDate(unsigned long *state) {
  char text[96];
  unsigned long day = nextRandom(state);
  snprintf(text, sizeof(text), "%s, %02lu %s %lu %02lu:%02lu:%02lu GMT",
           days[day % 7], 1 + nextRandom(state) % 28,
           months[nextRandom(state) % 12], 1990 + nextRandom(state) % 100,
           nextRandom(state) % 24, nextRandom(state) % 60,
           nextRandom(state) % 60);
  mixNumber((long long)curl_getdate(text, NULL));
}

static void addCookie(CURL *easy, unsigned long index, unsigned long *state) {
  char text[384];
  /* Netscape cookie-file lines: Set-Cookie: lines would stop at libcurl's
     limit of 50 per transfer. The expiry is fixed (2100-01-01). */
  snprintf(text, sizeof(text),
           "%s.host%lu.example%lu.org\tTRUE\t/p%lu/q%lu\t%s\t4102444800\t"
           "c%lu\tv%lx-%lx",
           (nextRandom(state) & 1) ? "#HttpOnly_" : "",
           nextRandom(state) % 300, nextRandom(state) % 4,
           nextRandom(state) % 6, nextRandom(state) % 3,
           (nextRandom(state) & 1) ? "TRUE" : "FALSE", index % 2000,
           nextRandom(state), nextRandom(state));
  mixNumber(curl_easy_setopt(easy, CURLOPT_COOKIELIST, text));
}

static void readCookies(CURL *easy) {
  struct curl_slist *cookies = NULL;
  struct curl_slist *each;
  long count = 0;

  mixNumber(curl_easy_getinfo(easy, CURLINFO_COOKIELIST, &cookies));
  for(each = cookies; each; each = each->next)
    count++;
  mixNumber(count);
  curl_slist_free_all(cookies);
}

int main(int argc, char **argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 10;
  unsigned long state = 1;
  int round;
  unsigned long i;
  CURL *easy;
  CURLU *url;

  if(curl_global_init(CURL_GLOBAL_DEFAULT))
    return 1;
  easy = curl_easy_init();
  url = curl_url();
  if(!easy || !url)
    return 1;
  for(round = 0; round < rounds; round++) {
    for(i = 0; i < URLS_PER_ROUND; i++) {
      parseUrl(url, &state);
      escapeText(easy, &state);
      parseDate(&state);
    }
    for(i = 0; i < COOKIES_PER_ROUND; i++)
      addCookie(easy, i, &state);
    readCookies(easy);
    mixNumber(curl_easy_setopt(easy, CURLOPT_COOKIELIST, "ALL"));
  }
  curl_url_cleanup(url);
  curl_easy_cleanup(easy);
  curl_global_cleanup();
  printf("curl-bench %d rounds checksum %016llx\n", rounds, checksum);
  return 0;
}
