/* loadgen.c: a native replay of bench/load.js for many more users than k6 fits in a small VM.
 * One epoll thread, one keep-alive connection per user, the same user loop and think times:
 *   GET /feed -> 3-7 s -> GET /posts/:id (one of the feed) -> 3-8 s -> 15% like, 2% post -> 5-15 s.
 * Users ramp up linearly over RAMP seconds, hold for DURATION, then stop.
 *
 *   loadgen <host> <port> <tokens.json> <users> <ramp_s> <duration_s> [rampdown_s, default 0 = stop at once]
 *
 * Pass/fail like k6: p95 < 500 ms, p99 < 1 s, errors < 1 %. Source addresses rotate over
 * 127.0.0.1..127.0.0.250 so one box can hold hundreds of thousands of connections.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

enum { S_IDLE = 0, S_CONNECTING, S_SENDING, S_RECEIVING };
enum { Q_FEED = 0, Q_POST, Q_LIKE, Q_CREATE, Q_N };
static const char *qname[Q_N] = {"GET /feed", "GET /posts/:id", "POST /posts/:id/like", "POST /posts"};

struct vu {
  int fd;
  uint8_t state, step, q, first;
  uint32_t iter;
  uint64_t wake_ns, sent_ns;
  int64_t post_id;
  uint32_t buf_i; /* index into the buffer pool while a response is in flight, or 0 */
  uint32_t req_off, req_len;
  uint32_t resp_n, resp_need;
  uint32_t next; /* timer wheel link: index + 1, 0 = end */
  uint8_t stopped; /* ramp-down: this user has left */
};

/* timer wheel: 10 ms slots over a 40 s horizon (think times are at most 15 s + 5 s) */
#define WHEEL_SLOT_NS 10000000ULL
#define WHEEL_SLOTS 4096
static uint32_t wheel[WHEEL_SLOTS];
static uint64_t wheel_cur; /* slot index (absolute) processed so far */
static struct vu *vus;

static void wheel_add(struct vu *v) {
  uint64_t slot = v->wake_ns / WHEEL_SLOT_NS;
  if (slot <= wheel_cur) slot = wheel_cur + 1;
  uint32_t i = (uint32_t)(slot % WHEEL_SLOTS);
  v->next = wheel[i];
  wheel[i] = (uint32_t)(v - vus) + 1;
}

/* A request is built and sent from one scratch buffer and a response read into another; a VU takes
 * a pool buffer only for a send or a response that does not finish in one call (rare on loopback),
 * so memory stays flat however many requests are in flight. The pool grows in chunks that never
 * move. */
#define BUFSZ 8192
struct buf {
  uint32_t next_free;
  uint8_t data[BUFSZ];
};
#define CHUNK 1024
static struct buf *chunks[1 << 12];
#define BUF(i) (&chunks[(i) / CHUNK][(i) % CHUNK])
static uint8_t sbuf[BUFSZ], rbuf[BUFSZ];

static int nvus;
static uint32_t nbufs, free_buf;
static char **tokens, **names;
static int ntokens;
static struct sockaddr_in server;
static struct in_addr srcs[64]; /* LG_SRC=ip,ip,...: source addresses, round robin */
static int nsrc;
static int epfd;
static uint64_t t_start;

/* latency histogram: 50 µs buckets to 10 s, then an overflow */
#define NB 200001
static uint32_t hist[Q_N][NB];
static uint64_t n_req[Q_N], n_err[Q_N], n_bad[Q_N], sum_ns[Q_N], max_ns[Q_N];
static uint64_t n_close_hdr, n_slow_connect, n_connect_err, n_eof, n_timeout, n_total, n_errs, n_idle_close, n_reconnect;
static uint32_t hist_all[NB];
/* the same, per 10 s report window, so a ramp shows where it breaks */
static uint32_t hist_win[NB];
static uint64_t win_total, win_errs;
static int n_open; /* sockets connected or connecting */

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static double frand(void) { return (double)rand() / ((double)RAND_MAX + 1.0); }
static double think_scale = 1.0; /* < 1: users think faster, so the same users send more requests */
static uint64_t between_ns(double lo, double hi) { return (uint64_t)((lo + frand() * (hi - lo)) * think_scale * 1e9); }

static uint32_t buf_get(void) {
  if (!free_buf) {
    uint32_t c = nbufs / CHUNK;
    if (c >= sizeof chunks / sizeof *chunks || !(chunks[c] = malloc(CHUNK * sizeof(struct buf)))) { perror("buf chunk"); exit(1); }
    for (uint32_t i = nbufs; i < nbufs + CHUNK; i++) BUF(i)->next_free = i + 1 < nbufs + CHUNK ? i + 1 : 0;
    free_buf = nbufs ? nbufs : 1; /* index 0 means "no buffer" */
    nbufs += CHUNK;
  }
  uint32_t b = free_buf;
  free_buf = BUF(b)->next_free;
  return b;
}
static void buf_put(uint32_t b) {
  BUF(b)->next_free = free_buf;
  free_buf = b;
}

static uint64_t n_slow, max_loop_ns;
static void record(struct vu *v, int status) {
  uint64_t dt = now_ns() - v->sent_ns;
  int q = v->q;
  if (dt > 100000000ULL && n_slow++ < 12)
    fprintf(stderr, "slow: %s %.1f ms vu=%ld iter=%u fd=%d status=%d resp_n=%u\n", qname[q], (double)dt / 1e6, (long)(v - vus), v->iter, v->fd, status, v->resp_n);
  n_req[q]++;
  n_total++;
  sum_ns[q] += dt;
  if (dt > max_ns[q]) max_ns[q] = dt;
  uint64_t b = dt / 50000;
  if (b >= NB) b = NB - 1;
  hist[q][b]++;
  hist_all[b]++;
  hist_win[b]++;
  win_total++;
  int ok = (q == Q_FEED || q == Q_POST) ? status == 200 : (q == Q_LIKE ? (status == 200 || status == 201) : status == 201);
  if (!ok) { n_bad[q]++; n_errs++; win_errs++; }
}

static void vu_close(struct vu *v) {
  if (v->fd >= 0) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, v->fd, NULL);
    close(v->fd);
    v->fd = -1;
    n_open--;
  }
  if (v->buf_i) { buf_put(v->buf_i); v->buf_i = 0; }
}

static void vu_fail(struct vu *v, int q_err) {
  if (q_err) { n_err[v->q]++; n_errs++; n_total++; win_errs++; win_total++; }
  vu_close(v);
  v->state = S_IDLE;
  if (v->stopped) return;
  v->step = 0;
  v->wake_ns = now_ns() + between_ns(3, 7); /* like load.js after a failed feed */
  wheel_add(v);
}

static void vu_build_request(struct vu *v, uint8_t *out, uint32_t *len) {
  int vu = (int)(v - vus);
  const char *tok = tokens[vu % ntokens], *name = names[vu % ntokens];
  int n;
  switch (v->q) {
  case Q_FEED: n = snprintf((char *)out, BUFSZ, "GET /feed HTTP/1.1\r\nHost: s\r\nUser-Agent: loadgen\r\nAccept: */*\r\n\r\n"); break;
  case Q_POST: n = snprintf((char *)out, BUFSZ, "GET /posts/%lld HTTP/1.1\r\nHost: s\r\nUser-Agent: loadgen\r\nAccept: */*\r\n\r\n", (long long)v->post_id); break;
  case Q_LIKE:
    n = snprintf((char *)out, BUFSZ, "POST /posts/%lld/like HTTP/1.1\r\nHost: s\r\nUser-Agent: loadgen\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n", (long long)v->post_id, tok);
    break;
  default: {
    char body[256];
    time_t t = time(NULL);
    char ts[40];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S.000Z", gmtime(&t));
    int bn = snprintf(body, sizeof body, "{\"body\":\"%s says hi at %s (VU %d, iter %u)\"}", name, ts, vu + 1, v->iter);
    n = snprintf((char *)out, BUFSZ, "POST /posts HTTP/1.1\r\nHost: s\r\nUser-Agent: loadgen\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", tok, bn, body);
  }
  }
  *len = (uint32_t)n;
}

static int vu_connect(struct vu *v) {
  int vu = (int)(v - vus);
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in src = {.sin_family = AF_INET, .sin_port = 0};
  /* A remote server: one source address holds ~64k connections to one port, so users are spread
   * over the LG_SRC addresses, and each user gets its own source port as well. With port 0 the
   * kernel searches for a free port at connect() (__inet_hash_connect), a search that grows with the
   * ports in use: at 212k connections and 5,700 new ones a second it stalled this one thread.
   * A fixed port is a single check. If it is still held (the user's last connection closing), the
   * kernel picks one as before. */
  unsigned port = nsrc ? 1024u + (unsigned)vu / (unsigned)nsrc : 0;
  if (nsrc && port <= 65535) {
    src.sin_addr = srcs[vu % nsrc];
    src.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&src, sizeof src) < 0) {
      src.sin_port = 0;
      setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof one);
      bind(fd, (struct sockaddr *)&src, sizeof src);
    }
  } else if (nsrc) {
    setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof one);
    src.sin_addr = srcs[vu % nsrc];
    bind(fd, (struct sockaddr *)&src, sizeof src);
  } else if (server.sin_addr.s_addr == htonl(0x7f000001u)) {
    setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof one);
    src.sin_addr.s_addr = htonl(0x7f000001u + (uint32_t)(vu % 250));
    bind(fd, (struct sockaddr *)&src, sizeof src);
  }
  if (connect(fd, (struct sockaddr *)&server, sizeof server) < 0 && errno != EINPROGRESS) { close(fd); return -1; }
  struct epoll_event ev = {.events = EPOLLOUT | EPOLLIN | EPOLLRDHUP, .data.u32 = (uint32_t)vu};
  epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
  v->fd = fd;
  n_open++;
  v->state = S_CONNECTING;
  return 0;
}

/* Start the VU's next request (its step in the loop). */
static void vu_fire(struct vu *v) {
  v->req_len = 0; /* built when the socket is writable */
  v->req_off = 0;
  v->resp_n = v->resp_need = 0;
  v->sent_ns = now_ns();
  if (v->fd < 0) {
    if (v->iter || v->step) n_reconnect++;
    if (vu_connect(v) < 0) { n_connect_err++; vu_fail(v, 1); return; }
    return; /* send on EPOLLOUT */
  }
  v->state = S_SENDING;
  struct epoll_event ev = {.events = EPOLLOUT | EPOLLIN | EPOLLRDHUP, .data.u32 = (uint32_t)(v - vus)};
  epoll_ctl(epfd, EPOLL_CTL_MOD, v->fd, &ev);
}

static void vu_send(struct vu *v) {
  uint8_t *data = v->buf_i ? BUF(v->buf_i)->data : sbuf;
  if (!v->req_len) vu_build_request(v, sbuf, &v->req_len);
  while (v->req_off < v->req_len) {
    ssize_t w = send(v->fd, data + v->req_off, v->req_len - v->req_off, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EAGAIN) {
        if (!v->buf_i) { /* keep the rest for the next EPOLLOUT */
          v->buf_i = buf_get();
          memcpy(BUF(v->buf_i)->data, sbuf, v->req_len);
        }
        return;
      }
      n_eof++;
      vu_fail(v, 1);
      return;
    }
    v->req_off += (uint32_t)w;
  }
  if (v->buf_i) { buf_put(v->buf_i); v->buf_i = 0; }
  v->state = S_RECEIVING;
  v->resp_n = 0;
  struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.u32 = (uint32_t)(v - vus)};
  epoll_ctl(epfd, EPOLL_CTL_MOD, v->fd, &ev);
}

/* After a complete response: schedule the next step. */
static void vu_done(struct vu *v, int status, const uint8_t *body, uint32_t body_n) {
  record(v, status);
  int ok = status >= 200 && status < 300;
  if (v->buf_i) { buf_put(v->buf_i); v->buf_i = 0; }
  v->state = S_IDLE;
  if (v->stopped) { vu_close(v); return; }
  uint64_t t = now_ns();
  switch (v->step) {
  case 0: /* feed */
    if (!ok) { v->step = 0; v->wake_ns = t + between_ns(3, 7); wheel_add(v); return; }
    {
      /* pick one of the feed's ids */
      int64_t ids[20];
      int n = 0;
      const char *p = (const char *)body, *end = p + body_n;
      while (n < 20 && (p = memmem(p, (size_t)(end - p), "\"id\":", 5))) {
        p += 5;
        ids[n++] = strtoll(p, NULL, 10);
      }
      v->post_id = n ? ids[rand() % n] : 1;
    }
    v->step = 1;
    v->wake_ns = t + between_ns(3, 7);
    wheel_add(v);
    return;
  case 1: /* post */
    v->step = 2;
    v->wake_ns = t + between_ns(3, 8);
    wheel_add(v);
    return;
  case 2: /* like done */
    v->step = 3;
    v->wake_ns = t;
    wheel_add(v);
    return;
  default: /* create done */
    v->step = 4;
    v->wake_ns = t;
    wheel_add(v);
    return;
  }
}

static void vu_recv(struct vu *v) {
  for (;;) {
    uint8_t *data = v->buf_i ? BUF(v->buf_i)->data : rbuf; /* rbuf holds nothing between calls */
    ssize_t r = recv(v->fd, data + v->resp_n, BUFSZ - 1 - v->resp_n, 0);
    if (r < 0) {
      if (errno == EAGAIN) {
        if (!v->buf_i && v->resp_n) { /* a partial response: keep it until the rest arrives */
          v->buf_i = buf_get();
          memcpy(BUF(v->buf_i)->data, rbuf, v->resp_n);
        }
        return;
      }
      n_eof++;
      vu_fail(v, 1);
      return;
    }
    if (r == 0) { n_eof++; vu_fail(v, 1); return; }
    v->resp_n += (uint32_t)r;
    data[v->resp_n] = 0;
    if (!v->resp_need) {
      char *hend = memmem(data, v->resp_n, "\r\n\r\n", 4);
      if (!hend) {
        if (v->resp_n >= BUFSZ - 1) { vu_fail(v, 1); return; }
        continue;
      }
      char *cl = strcasestr((char *)data, "content-length:");
      uint32_t need = cl ? (uint32_t)strtoul(cl + 15, NULL, 10) : 0;
      uint32_t head = (uint32_t)(hend + 4 - (char *)data);
      v->resp_need = head + need;
      if (v->resp_need >= BUFSZ) { vu_fail(v, 1); return; }
    }
    if (v->resp_n >= v->resp_need) {
      int status = atoi((char *)data + 9);
      char *hend = memmem(data, v->resp_n, "\r\n\r\n", 4);
      uint32_t head = (uint32_t)(hend + 4 - (char *)data);
      /* "Connection: close": the client drops the connection once the response is read and dials
       * again for its next request, as Go's http.Transport (k6) does */
      char save = hend[2];
      hend[2] = 0;
      int closing = strcasestr((char *)data, "\r\nconnection: close") != NULL;
      hend[2] = save;
      vu_done(v, status, data + head, v->resp_need - head);
      if (closing && v->fd >= 0) { n_close_hdr++; vu_close(v); }
      return;
    }
  }
}

/* The user loop: what to do when a VU wakes up. */
static void vu_tick(struct vu *v) {
  if (v->stopped) return;
  switch (v->step) {
  case 0: v->q = Q_FEED; vu_fire(v); return;
  case 1: v->q = Q_POST; vu_fire(v); return;
  case 2:
    if (frand() < 0.15) { v->q = Q_LIKE; vu_fire(v); return; }
    v->step = 3; /* fall through to the create decision */
    /* fallthrough */
  case 3:
    if (frand() < 0.02) { v->q = Q_CREATE; vu_fire(v); return; }
    v->step = 4;
    /* fallthrough */
  default:
    v->iter++;
    v->step = 0;
    v->wake_ns = now_ns() + between_ns(5, 15);
    wheel_add(v);
    return;
  }
}

/* Runs every slot up to now: wakes the users whose time has come. */
static void wheel_run(uint64_t now) {
  uint64_t target = now / WHEEL_SLOT_NS;
  while (wheel_cur < target) {
    wheel_cur++;
    uint32_t i = (uint32_t)(wheel_cur % WHEEL_SLOTS), head = wheel[i];
    wheel[i] = 0;
    while (head) {
      struct vu *v = &vus[head - 1];
      head = v->next;
      v->next = 0;
      if (v->state != S_IDLE) continue; /* stale entry: the user moved on */
      if (v->wake_ns > now) { wheel_add(v); continue; }
      vu_tick(v);
    }
  }
}

static double pct(uint32_t *h, uint64_t total, double p) {
  uint64_t target = (uint64_t)(p * (double)total), acc = 0;
  for (uint32_t i = 0; i < NB; i++) {
    acc += h[i];
    if (acc >= target) return i * 0.05;
  }
  return 10000;
}

static void report(int final) {
  uint64_t tot = 0;
  for (int q = 0; q < Q_N; q++) tot += n_req[q];
  double elapsed = (double)(now_ns() - t_start) / 1e9;
  fprintf(stderr, "[%6.0fs] reqs=%llu (%.0f/s) errors=%llu (%.2f%%) p50=%.1f p95=%.1f p99=%.1f ms conn_err=%llu eof=%llu timeout=%llu idle_close=%llu reconnect=%llu close_hdr=%llu slow_connect=%llu max_loop=%.0fms\n",
          elapsed, (unsigned long long)n_total, elapsed > 0 ? (double)n_total / elapsed : 0, (unsigned long long)n_errs,
          n_total ? 100.0 * (double)n_errs / (double)n_total : 0, pct(hist_all, tot, 0.5), pct(hist_all, tot, 0.95),
          pct(hist_all, tot, 0.99), (unsigned long long)n_connect_err, (unsigned long long)n_eof, (unsigned long long)n_timeout, (unsigned long long)n_idle_close, (unsigned long long)n_reconnect, (unsigned long long)n_close_hdr, (unsigned long long)n_slow_connect, (double)max_loop_ns / 1e6);
  max_loop_ns = 0;
  if (!final) {
    static uint64_t t_last;
    uint64_t t = now_ns();
    double dt = t_last ? (double)(t - t_last) / 1e9 : elapsed;
    t_last = t;
    fprintf(stderr, "win: open=%d req/s=%.0f errors=%.2f%% p95=%.1f p99=%.1f ms\n", n_open, (double)win_total / dt,
            win_total ? 100.0 * (double)win_errs / (double)win_total : 0, pct(hist_win, win_total, 0.95), pct(hist_win, win_total, 0.99));
    /* three windows of nothing but errors: the server is gone, and 70k retries a second only jam the
     * VMs' network (LG_KEEP_ON_DEAD=1 keeps going) */
    static int dead;
    dead = win_total && win_errs == win_total ? dead + 1 : 0;
    memset(hist_win, 0, sizeof hist_win);
    win_total = win_errs = 0;
    if (dead >= 3 && !getenv("LG_KEEP_ON_DEAD")) { fprintf(stderr, "server gone: stopping\n"); report(1); exit(0); }
    return;
  }
  for (int q = 0; q < Q_N; q++) {
    if (!n_req[q]) continue;
    fprintf(stderr, "  %-22s n=%-8llu bad=%-6llu err=%-6llu avg=%.2f p95=%.1f p99=%.1f max=%.1f ms\n", qname[q],
            (unsigned long long)n_req[q], (unsigned long long)n_bad[q], (unsigned long long)n_err[q],
            (double)sum_ns[q] / (double)n_req[q] / 1e6, pct(hist[q], n_req[q], 0.95), pct(hist[q], n_req[q], 0.99),
            (double)max_ns[q] / 1e6);
  }
  double p95 = pct(hist_all, tot, 0.95), p99 = pct(hist_all, tot, 0.99);
  double errp = n_total ? 100.0 * (double)n_errs / (double)n_total : 0;
  int pass = p95 < 500 && p99 < 1000 && errp < 1.0;
  fprintf(stderr, "RESULT: %s (p95 %.1f ms, p99 %.1f ms, errors %.2f%%)\n", pass ? "PASS" : "FAIL", p95, p99, errp);
}

static int load_tokens(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); return -1; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *s = malloc((size_t)n + 1);
  if (fread(s, 1, (size_t)n, f) != (size_t)n) return -1;
  s[n] = 0;
  fclose(f);
  int cap = 1024;
  tokens = malloc((size_t)cap * sizeof *tokens);
  names = malloc((size_t)cap * sizeof *names);
  char *p = s;
  while ((p = strstr(p, "\"username\":\""))) {
    p += 12;
    char *e = strchr(p, '"');
    *e = 0;
    char *name = p;
    p = strstr(e + 1, "\"token\":\"") + 9;
    e = strchr(p, '"');
    *e = 0;
    if (ntokens == cap) {
      cap *= 2;
      tokens = realloc(tokens, (size_t)cap * sizeof *tokens);
      names = realloc(names, (size_t)cap * sizeof *names);
    }
    tokens[ntokens] = p;
    names[ntokens] = name;
    ntokens++;
    p = e + 1;
  }
  return ntokens ? 0 : -1;
}

int main(int argc, char **argv) {
  if (argc < 7) { fprintf(stderr, "usage: %s host port tokens.json users ramp_s duration_s\n", argv[0]); return 2; }
  nvus = atoi(argv[4]);
  double ramp = atof(argv[5]), duration = atof(argv[6]), rampdown = argc > 7 ? atof(argv[7]) : 0;
  if (argc > 8 && atof(argv[8]) > 0) think_scale = atof(argv[8]);
  if (load_tokens(argv[3]) < 0) return 1;
  server.sin_family = AF_INET;
  server.sin_port = htons((uint16_t)atoi(argv[2]));
  inet_pton(AF_INET, argv[1], &server.sin_addr);
  const char *lg_src = getenv("LG_SRC");
  for (const char *p = lg_src; p && *p && nsrc < 64;) {
    char ip[32];
    size_t k = strcspn(p, ",");
    snprintf(ip, sizeof ip, "%.*s", (int)k, p);
    if (inet_pton(AF_INET, ip, &srcs[nsrc]) == 1) nsrc++;
    p += k + (p[k] == ',');
  }
  struct rlimit rl;
  getrlimit(RLIMIT_NOFILE, &rl);
  rl.rlim_cur = rl.rlim_max;
  setrlimit(RLIMIT_NOFILE, &rl);
  if ((int)rl.rlim_cur < nvus + 64) fprintf(stderr, "warning: fd limit %lu < users %d\n", (unsigned long)rl.rlim_cur, nvus);
  /* the load generator shares the VM with the server; locked, its own pages are never evicted and
   * re-read near the memory wall, which would show up as latency the server did not cause */
  if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) perror("mlockall");
  srand(1);
  vus = calloc((size_t)nvus, sizeof *vus);
  for (int i = 0; i < nvus; i++) vus[i].fd = -1;
  epfd = epoll_create1(EPOLL_CLOEXEC);
  t_start = now_ns();
  uint64_t t_end_ramp = t_start + (uint64_t)(ramp * 1e9), t_end = t_end_ramp + (uint64_t)(duration * 1e9);
  int started = 0, active = nvus;
  uint64_t rampdown_ns = (uint64_t)(rampdown * 1e9), t_done = t_end + rampdown_ns;
  uint64_t last_report = t_start, last_sweep = t_start;
  wheel_cur = t_start / WHEEL_SLOT_NS;
  static struct epoll_event evs[4096];
  int stopping = 0;
  uint64_t t_prev = now_ns();
  for (;;) {
    uint64_t t = now_ns();
    if (t - t_prev > max_loop_ns) max_loop_ns = t - t_prev;
    t_prev = t;
    /* ramp: users start linearly; each spreads its first iteration over 0-5 s like load.js */
    int want = t >= t_end_ramp ? nvus : (int)((double)nvus * (double)(t - t_start) / (double)(t_end_ramp - t_start));
    while (started < want) {
      vus[started].wake_ns = t + between_ns(0, 5);
      vus[started].step = 0;
      wheel_add(&vus[started]);
      started++;
    }
    /* ramp-down, like load.js's last stage: users leave linearly, an idle one at once, a busy one
     * after its response */
    if (t >= t_end) {
      int keep = t >= t_done || !rampdown_ns ? 0 : (int)((double)nvus * (1.0 - (double)(t - t_end) / (double)rampdown_ns));
      while (active > keep) {
        struct vu *v = &vus[--active];
        v->stopped = 1;
        if (v->state == S_IDLE) vu_close(v);
      }
    }
    if (t >= t_done && !stopping) { stopping = 1; report(1); break; }
    wheel_run(t);
    if (t - last_sweep > 1000000000ULL) { /* 60 s request timeouts, like k6 */
      last_sweep = t;
      for (int i = 0; i < started; i++) {
        struct vu *v = &vus[i];
        if (v->state != S_IDLE && t > v->sent_ns && t - v->sent_ns > 60000000000ULL) { n_timeout++; vu_fail(v, 1); }
      }
    }
    int n = epoll_wait(epfd, evs, 4096, 1);
    for (int i = 0; i < n; i++) {
      struct vu *v = &vus[evs[i].data.u32];
      if (v->fd < 0) continue;
      if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
        if (v->state == S_CONNECTING) n_connect_err++; else n_eof++;
        vu_fail(v, 1);
        continue;
      }
      if (v->state == S_CONNECTING && (evs[i].events & EPOLLOUT)) {
        /* k6's http_req_duration starts when the connection is up (connecting is http_req_connecting) */
        uint64_t t_up = now_ns();
        if (t_up - v->sent_ns > 1000000000ULL) n_slow_connect++;
        v->sent_ns = t_up;
        v->state = S_SENDING;
        vu_send(v);
        continue;
      }
      if (v->state == S_SENDING && (evs[i].events & EPOLLOUT)) { vu_send(v); continue; }
      if (v->state == S_RECEIVING && (evs[i].events & (EPOLLIN | EPOLLRDHUP))) { vu_recv(v); continue; }
      if (v->state == S_IDLE && (evs[i].events & (EPOLLIN | EPOLLRDHUP))) { /* server closed an idle keep-alive */
        char junk[256];
        ssize_t r = recv(v->fd, junk, sizeof junk, 0);
        if (n_idle_close < 3) fprintf(stderr, "idle socket readable: recv=%zd%s%.*s\n", r, r > 0 ? " data=" : "", r > 0 ? (int)(r > 80 ? 80 : r) : 0, junk);
        n_idle_close++;
        vu_close(v);
      }
    }
    if (t - last_report > 10000000000ULL) { report(0); last_report = t; }
  }
  return 0;
}
