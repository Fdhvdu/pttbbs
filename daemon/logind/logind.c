// The High Performance Login Daemon (works with tunnel mode)
// $Id$
//
// Create:       Hung-Te Lin <piaip@csie.ntu.edu.tw>
// Contributors: wens, kcwu
// Initial Date: 2009/06/01
//
// Copyright (C) 2009, Hung-Te Lin <piaip@csie.ntu.edu.tw>
// All rights reserved

// TODO:
// 1. [done] cache guest's usernum and check if too many guests online
// 2. [drop] change close connection to 'wait until user hit then close'
// 3. [done] regular check text screen files instead of HUP?
// 4. [done] re-start mbbsd if pipe broken?
// 5. [drop] clean mbbsd pid log files?
// 6. [done] handle non-block i/o
// 7. [done] asynchronous tunnel handshake

#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <signal.h>
#include <event.h>

// XXX should we need to keep this definition?
#define _BBS_UTIL_C_

#include "bbs.h"
#include "banip.h"
#include "logind.h"

#ifndef LOGIND_REGULAR_CHECK_DURATION
#define LOGIND_REGULAR_CHECK_DURATION   (15)
#endif 

#ifndef LOGIND_MAX_FDS
#define LOGIND_MAX_FDS      (100000)
#endif

// some systems has hard limit of this to 128.
#ifndef LOGIND_SOCKET_QLEN
#define LOGIND_SOCKET_QLEN  (100)
#endif

#ifndef AUTHFAIL_SLEEP_SEC
#define AUTHFAIL_SLEEP_SEC  (15)
#endif

#ifndef OVERLOAD_SLEEP_SEC
#define OVERLOAD_SLEEP_SEC  (60)
#endif

#ifndef ACK_TIMEOUT_SEC
#define ACK_TIMEOUT_SEC     (30) // (5*60)
#endif

#ifndef BAN_SLEEP_SEC
#define BAN_SLEEP_SEC       (60)
#endif

#ifndef IDLE_TIMEOUT_SEC
#define IDLE_TIMEOUT_SEC    (20*60)
#endif

#ifndef MAX_TEXT_SCREEN_LINES
#define MAX_TEXT_SCREEN_LINES   (24)
#endif

#ifndef OPTIMIZE_SOCKET
#define OPTIMIZE_SOCKET(sock) do {} while(0)
#endif

// to prevent flood trying services...
#ifndef LOGIND_MAX_RETRY_SERVICE
#define LOGIND_MAX_RETRY_SERVICE   (15)
#endif

// local definiions
#define MY_SVC_NAME  "logind"
#define LOG_PREFIX  "[logind] "

///////////////////////////////////////////////////////////////////////
// global variables
int g_tunnel;           // tunnel for service daemon

// server status
int g_overload = 0;
int g_banned   = 0;
int g_verbose  = 0;
int g_opened_fd= 0;
int g_nonblock = 1;
int g_async_ack= 1;

// retry service
char g_retry_cmd[PATHLEN];
int  g_retry_times;

// cache data
int g_reload_data = 1;  // request to reload data
time4_t g_welcome_mtime;
int g_guest_usernum  = 0;  // numeric uid of guest account
int g_guest_too_many = 0;  // 1 if exceed MAX_GUEST

///////////////////////////////////////////////////////////////////////
// login context, constants and states

enum {
    LOGIN_STATE_START  = 1,
    LOGIN_STATE_USERID,
    LOGIN_STATE_PASSWD,
    LOGIN_STATE_AUTH,
    LOGIN_STATE_WAITACK,

    LOGIN_HANDLE_WAIT = 1,
    LOGIN_HANDLE_BEEP,
    LOGIN_HANDLE_OUTC,
    LOGIN_HANDLE_REDRAW_USERID,
    LOGIN_HANDLE_BS,
    LOGIN_HANDLE_PROMPT_PASSWD,
    LOGIN_HANDLE_START_AUTH,

    AUTH_RESULT_STOP   = -3,
    AUTH_RESULT_FREEID_TOOMANY = -2,
    AUTH_RESULT_FREEID = -1,
    AUTH_RESULT_FAIL   = 0,
    AUTH_RESULT_RETRY  = AUTH_RESULT_FAIL,
    AUTH_RESULT_OK     = 1,
};

#ifdef  CONVERT
#define IDBOXLEN    (IDLEN+2)   // one extra char for encoding
#else
#define IDBOXLEN    (IDLEN+1)
#endif

typedef struct {
    int  state;
    int  retry;
    int  encoding;
    int  t_lines;
    int  t_cols;
    int  icurr;         // cursor (only available in userid input mode)
    Fnv32_t client_code;
    char userid [IDBOXLEN];
    char pad0;   // for safety
    char passwd [PASSLEN+1];
    char pad1;   // for safety
    char hostip [IPV4LEN+1];
    char pad2;   // for safety
    char port   [IDLEN+1];
    char pad3;   // for safety
} login_ctx;

typedef struct {
    unsigned int cb;
    struct bufferevent *bufev;
    struct event ev;
    TelnetCtx    telnet;
    login_ctx    ctx;
} login_conn_ctx;

typedef struct {
    struct event ev;
    int    port;
} bind_event;

void 
login_ctx_init(login_ctx *ctx)
{
    assert(ctx);
    memset(ctx, 0, sizeof(login_ctx));
    ctx->client_code = FNV1_32_INIT;
    ctx->state = LOGIN_STATE_START;
}

int 
login_ctx_retry(login_ctx *ctx)
{
    assert(ctx);
    ctx->state = LOGIN_STATE_START;
    ctx->encoding = 0;
    memset(ctx->userid, 0, sizeof(ctx->userid));
    memset(ctx->passwd, 0, sizeof(ctx->passwd));
    ctx->icurr    = 0;
    // do not touch hostip, client code, t_*
    ctx->retry ++;
    return ctx->retry;
}

int 
login_ctx_handle(login_ctx *ctx, int c)
{
    int l;

    assert(ctx);
    switch(ctx->state)
    {
        case LOGIN_STATE_START:
        case LOGIN_STATE_USERID:
            l = strlen(ctx->userid);

            switch(c)
            {
                case KEY_ENTER:
                    ctx->state = LOGIN_STATE_PASSWD;
                    return LOGIN_HANDLE_PROMPT_PASSWD;

                case KEY_BS:
                    if (!l || !ctx->icurr)
                        return LOGIN_HANDLE_BEEP;
                    if (ctx->userid[ctx->icurr])
                    {
                        ctx->icurr--;
                        memmove(ctx->userid + ctx->icurr,
                                ctx->userid + ctx->icurr+1,
                                l - ctx->icurr);
                        return LOGIN_HANDLE_REDRAW_USERID;
                    }
                    // simple BS
                    ctx->icurr--;
                    ctx->userid[l-1] = 0;
                    return LOGIN_HANDLE_BS;

                case Ctrl('D'):
                case KEY_DEL:
                    if (!l || !ctx->userid[ctx->icurr])
                        return LOGIN_HANDLE_BEEP;
                    memmove(ctx->userid + ctx->icurr,
                            ctx->userid + ctx->icurr+1,
                            l - ctx->icurr);
                    return LOGIN_HANDLE_REDRAW_USERID;

                case Ctrl('B'):
                case KEY_LEFT:
                    if (ctx->icurr)
                        ctx->icurr--;
                    return LOGIN_HANDLE_REDRAW_USERID;

                case Ctrl('F'):
                case KEY_RIGHT:
                    if (ctx->userid[ctx->icurr])
                        ctx->icurr ++;
                    return LOGIN_HANDLE_REDRAW_USERID;

                case KEY_HOME:
                    ctx->icurr = 0;
                    return LOGIN_HANDLE_REDRAW_USERID;

                case KEY_END:
                    ctx->icurr = l;
                    return LOGIN_HANDLE_REDRAW_USERID;

                case Ctrl('K'):
                    if (!l || !ctx->userid[ctx->icurr])
                        return LOGIN_HANDLE_BEEP;
                    memset( ctx->userid + ctx->icurr, 0,
                            l - ctx->icurr +1);
                    return LOGIN_HANDLE_REDRAW_USERID;
            }

            // default: insert characters
            if (!isascii(c) || !isprint(c) || 
                c == ' ' ||
                l+1 >= sizeof(ctx->userid))
                return LOGIN_HANDLE_BEEP;

            memmove(ctx->userid + ctx->icurr + 1,
                    ctx->userid + ctx->icurr,
                    l - ctx->icurr +1);
            ctx->userid[ctx->icurr++] = c;

            if (ctx->icurr != l+1)
                return LOGIN_HANDLE_REDRAW_USERID;

            return LOGIN_HANDLE_OUTC;

        case LOGIN_STATE_PASSWD:
            l = strlen(ctx->passwd);

            if (c == KEY_ENTER)
            {
                // no matter what, apply the passwd
                ctx->state = LOGIN_STATE_AUTH;
                return LOGIN_HANDLE_START_AUTH;
            }
            if (c == KEY_BS)
            {
                if (!l)
                    return LOGIN_HANDLE_BEEP;
                ctx->passwd[l-1] = 0;
                return LOGIN_HANDLE_WAIT;
            }

            // XXX check VGET_PASSWD = VGET_NOECHO|VGET_ASCIIONLY
            if ( (!isascii(c) || !isprint(c)) || 
                l+1 >= sizeof(ctx->passwd))
                return LOGIN_HANDLE_BEEP;

            ctx->passwd[l] = c;

            return LOGIN_HANDLE_WAIT;

        default:
            break;
    }
    return LOGIN_HANDLE_BEEP;
}

///////////////////////////////////////////////////////////////////////
// Mini Queue

#define ACK_QUEUE_DEFAULT_CAPACITY  (128)
static login_conn_ctx **g_ack_queue;
static size_t           g_ack_queue_size,
                        g_ack_queue_reuse,
                        g_ack_queue_capacity;

static void
ackq_gc()
{
    // reset queue to zero if already empty.
    if (g_ack_queue_reuse == g_ack_queue_size)
        g_ack_queue_reuse =  g_ack_queue_size = 0;
}

static void
ackq_add(login_conn_ctx *ctx)
{
    assert(ctx->cb == sizeof(login_conn_ctx));
    if (g_ack_queue_reuse)
    {
        // there's some space in the queue, let's use it.
        size_t i;
        for (i = 0; i < g_ack_queue_size; i++)
        {
            if (g_ack_queue[i])
                continue;

            g_ack_queue[i] = ctx;
            g_ack_queue_reuse--;
            ackq_gc();
            return;
        }
        assert(!"corrupted ack queue");
        // may cause leak here, since queue is corrupted.
        return;
    }

    if (++g_ack_queue_size > g_ack_queue_capacity)
    {
        g_ack_queue_capacity *= 2;
        if (g_ack_queue_capacity < ACK_QUEUE_DEFAULT_CAPACITY)
            g_ack_queue_capacity = ACK_QUEUE_DEFAULT_CAPACITY;

        fprintf(stderr, LOG_PREFIX "resize ack queue to: %u (%u in use)\r\n",
                (unsigned int)g_ack_queue_capacity, (unsigned int)g_ack_queue_size);

        g_ack_queue = (login_conn_ctx**) realloc (g_ack_queue, 
                sizeof(login_conn_ctx*) * g_ack_queue_capacity);
        assert(g_ack_queue);
    }
    g_ack_queue[g_ack_queue_size-1] = ctx;
    ackq_gc();
}

static int
ackq_del(login_conn_ctx *ctx)
{
    size_t i;

    assert(ctx && ctx->cb == sizeof(login_conn_ctx));
    for (i = 0; i < g_ack_queue_size; i++)
    {
        if (g_ack_queue[i] != ctx)
            continue;

        // found the target
        g_ack_queue[i] = NULL;

        if (i+1 == g_ack_queue_size)
            g_ack_queue_size--;
        else
            g_ack_queue_reuse++;

        ackq_gc();
        return 1;
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////
// I/O

static ssize_t 
_buff_write(login_conn_ctx *conn, const void *buf, size_t nbytes)
{
    return bufferevent_write(conn->bufev, buf, nbytes);
}

///////////////////////////////////////////////////////////////////////
// Mini Terminal

static void 
_mt_bell(login_conn_ctx *conn)
{
    static const char b = Ctrl('G');
    _buff_write(conn, &b, 1);
}

static void 
_mt_bs(login_conn_ctx *conn)
{
    static const char cmd[] = "\b \b";
    _buff_write(conn, cmd, sizeof(cmd)-1);
}

static void 
_mt_home(login_conn_ctx *conn)
{
    static const char cmd[] = ESC_STR "[H";
    _buff_write(conn, cmd, sizeof(cmd)-1);
}

static void 
_mt_clrtoeol(login_conn_ctx *conn)
{
    static const char cmd[] = ESC_STR "[K";
    _buff_write(conn, cmd, sizeof(cmd)-1);
}

static void 
_mt_clear(login_conn_ctx *conn)
{
    static const char cmd[] = ESC_STR "[2J";
    _mt_home(conn);
    _buff_write(conn, cmd, sizeof(cmd)-1);
}

static void 
_mt_move_yx(login_conn_ctx *conn, const char *mcmd)
{
    static const char cmd1[] = ESC_STR "[",
                      cmd2[] = "H";
    _buff_write(conn, cmd1, sizeof(cmd1)-1);
    _buff_write(conn, mcmd, strlen(mcmd));
    _buff_write(conn, cmd2, sizeof(cmd2)-1);
}

///////////////////////////////////////////////////////////////////////
// ANSI/vt100/vt220 special keys

static int
_handle_term_keys(char **pstr, int *plen)
{
    char *str = *pstr;

    assert(plen && pstr && *pstr && *plen > 0);
    // fprintf(stderr, "handle_term: input = %d\r\n", *plen);

    // 1. check ESC
    (*plen)--; (*pstr)++;
    if (*str != ESC_CHR)
    {
        int c = (unsigned char)*str;

        switch(c)
        {
            case KEY_CR:
                return KEY_ENTER;

            case KEY_LF:
                return 0; // to ignore

            case Ctrl('A'):
                return KEY_HOME;
            case Ctrl('E'):
                return KEY_END;

            // case '\b':
            case Ctrl('H'):
            case 127:
                return KEY_BS;
        }
        return c;
    }

    // 2. check O / [
    if (!*plen)
        return KEY_ESC;
    (*plen)--; (*pstr)++; str++;
    if (*str != 'O' && *str != '[')
        return *str;
    // 3. alpha: end, digit: one more (~)
    if (!*plen)
        return *str;
    (*plen)--; (*pstr)++; str++;
    if (!isascii(*str))
        return KEY_UNKNOWN;
    if (isdigit(*str))
    {
        if (*plen)
        {
            (*plen)--; (*pstr)++;
        }
        switch(*str)
        {
            case '1':
                // fprintf(stderr, "got KEY_HOME.\r\n");
                return KEY_HOME;
            case '4':
                // fprintf(stderr, "got KEY_END.\r\n");
                return KEY_END;
            case '3':
                // fprintf(stderr, "got KEY_DEL.\r\n");
                return KEY_DEL;
            default:
                // fprintf(stderr, "got KEY_UNKNOWN.\r\n");
                return KEY_UNKNOWN;
        }
    }
    if (isalpha(*str))
    {
        switch(*str)
        {
            case 'C':
                // fprintf(stderr, "got KEY_RIGHT.\r\n");
                return KEY_RIGHT;
            case 'D':
                // fprintf(stderr, "got KEY_LEFT.\r\n");
                return KEY_LEFT;
            default:
                return KEY_UNKNOWN;
        }
    }

    // unknown
    return KEY_UNKNOWN;
}

///////////////////////////////////////////////////////////////////////
// Telnet Protocol

static void 
_telnet_resize_term_cb(void *resize_arg, int w, int h)
{
    login_ctx *ctx = (login_ctx*) resize_arg;
    assert(ctx);
    ctx->t_lines = h;
    ctx->t_cols  = w;
}

static void 
_telnet_update_cc_cb(void *cc_arg, unsigned char c)
{
    login_ctx *ctx = (login_ctx*) cc_arg;
    assert(ctx);
    // fprintf(stderr, "update cc: %08lX", (unsigned long)ctx->client_code);
    FNV1A_CHAR(c, ctx->client_code);
    // fprintf(stderr, "-> %08lX\r\n", (unsigned long)ctx->client_code);
}

static void 
_telnet_write_data_cb(void *write_arg, int fd, const void *buf, size_t nbytes)
{
    login_conn_ctx *conn = (login_conn_ctx *)write_arg;
    _buff_write(conn, buf, nbytes);
}

#ifdef  LOGIND_OPENFD_IN_AYT
static void 
_telnet_send_ayt_cb(void *ayt_arg, int fd)
{
    login_conn_ctx *conn = (login_conn_ctx *)ayt_arg;
    char buf[64];

    assert(conn);
    if (!g_async_ack)
    {
        snprintf(buf, sizeof(buf), "  (#%d)fd:%u  \r\n", 
                g_retry_times, g_opened_fd);
    }
    else
    {
        snprintf(buf, sizeof(buf), "  (#%d)fd:%u,ack:%u(-%u)  \r\n", 
                g_retry_times, g_opened_fd, 
                (unsigned int)g_ack_queue_size, 
                (unsigned int)g_ack_queue_reuse );
    }
    _buff_write(conn, buf, strlen(buf));
}
#endif

const static struct TelnetCallback 
telnet_callback = {
    _telnet_write_data_cb,
    _telnet_resize_term_cb,

#ifdef DETECT_CLIENT
    _telnet_update_cc_cb,
#else
    NULL,
#endif

#ifdef LOGIND_OPENFD_IN_AYT
    _telnet_send_ayt_cb,
#else
    NULL,
#endif
};

///////////////////////////////////////////////////////////////////////
// Socket Option

static void
_enable_nonblock(int sock)
{
    // XXX note: NONBLOCK is not always inherited (eg, not on Linux).
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
}

static void
_disable_nonblock(int sock)
{
    // XXX note: NONBLOCK is not always inherited (eg, not on Linux).
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) & (~O_NONBLOCK) );
}

static int 
_set_connection_opt(int sock)
{
    const int szrecv = 1024, szsend=4096;
    const struct linger lin = {0};

    // keep alive: server will check target connection. (around 2hr)
    const int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void*)&on, sizeof(on));
   
    // fast close
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
    // adjust transmission window
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*)&szrecv, sizeof(szrecv));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (void*)&szsend, sizeof(szsend));
    OPTIMIZE_SOCKET(sock);

    return 0;
}

static int
_set_bind_opt(int sock)
{
    const int on = 1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*)&on, sizeof(on));
    _set_connection_opt(sock);

    if (g_nonblock)
        _enable_nonblock(sock);

    return 0;
}

///////////////////////////////////////////////////////////////////////
// Draw Screen

#ifndef  INSCREEN
# define INSCREEN ANSI_RESET "\r\n【" BBSNAME "】◎(" MYHOSTNAME ", " MYIP ") \r\n"
#endif

#ifdef   STR_GUEST
# define MSG_GUEST "，或以[" STR_GUEST "]參觀"
#else
# define MSG_GUEST ""
#endif

#ifdef   STR_REGNEW
# define MSG_REGNEW "，或以[new]註冊"
#else
# define MSG_REGNEW
#endif

#define BOTTOM_YX           "24;1"
#define LOGIN_PROMPT_MSG    ANSI_RESET "請輸入代號" MSG_GUEST MSG_REGNEW ": " ANSI_REVERSE
#define LOGIN_PROMPT_YX     "21;1"
#define LOGIN_PROMPT_END    ANSI_RESET
#define PASSWD_PROMPT_MSG   ANSI_RESET MSG_PASSWD
#define PASSWD_PROMPT_YX    "22;1"
#define PASSWD_CHECK_MSG    ANSI_RESET "正在檢查帳號與密碼..."
#define PASSWD_CHECK_YX     PASSWD_PROMPT_YX
#define AUTH_SUCCESS_MSG    ANSI_RESET "密碼正確！ 開始登入系統...\r\n"
#define AUTH_SUCCESS_YX     PASSWD_PROMPT_YX
#define FREEAUTH_SUCCESS_MSG    ANSI_RESET "開始登入系統...\r\n"
#define FREEAUTH_SUCCESS_YX AUTH_SUCCESS_YX
#define AUTH_FAIL_MSG       ANSI_RESET ERR_PASSWD
#define AUTH_FAIL_YX        PASSWD_PROMPT_YX
#define USERID_EMPTY_MSG    ANSI_RESET "請重新輸入。"
#define USERID_EMPTY_YX     PASSWD_PROMPT_YX
#define SERVICE_FAIL_MSG    ANSI_COLOR(0;1;31) "抱歉，部份系統正在維護中，請稍候再試。 " ANSI_RESET
#define SERVICE_FAIL_YX     BOTTOM_YX
#define OVERLOAD_CPU_MSG    ANSI_RESET " 系統過載, 請稍後再來... "
#define OVERLOAD_CPU_YX     BOTTOM_YX
#define OVERLOAD_USER_MSG   ANSI_RESET " 由於人數過多，請您稍後再來... "
#define OVERLOAD_USER_YX    BOTTOM_YX

#define REJECT_FREE_UID_MSG ANSI_RESET " 抱歉，此帳號或服務已達上限。 "
#define REJECT_FREE_UID_YX  BOTTOM_YX

#ifdef  STR_GUEST
#define TOO_MANY_GUEST_MSG  ANSI_RESET " 抱歉，目前已有太多 " STR_GUEST " 在站上。 "
#define TOO_MANY_GUEST_YX   BOTTOM_YX
#endif

#define FN_WELCOME          BBSHOME "/etc/Welcome"
#define FN_GOODBYE          BBSHOME "/etc/goodbye"
#define FN_BAN              BBSHOME "/" BAN_FILE

static char *welcome_screen, *goodbye_screen, *ban_screen;

static void
load_text_screen_file(const char *filename, char **pptr)
{
    FILE *fp;
    off_t sz, wsz, psz;
    char *p, *s = NULL;
    int max_lines = MAX_TEXT_SCREEN_LINES;

    sz = dashs(filename);
    if (sz < 1)
    {
        free(*pptr);
        *pptr = NULL;
        return;
    }
    wsz = sz*2 +1; // *2 for cr+lf, extra one byte for safe strchr().

    assert(pptr);
    s = *pptr;
    s = realloc(s, wsz);  
    *pptr = s;
    if (!s)
        return;

    memset(s, 0, wsz);
    p = s;
    psz = wsz;

    fp = fopen(filename, "rt");
    if (!fp)
    {
        free(s);
        return;
    }
    while ( max_lines-- > 0 &&
            fgets(p, psz, fp))
    {
        psz -= strlen(p);
        p += strlen(p);
        *p ++ = '\r';
    }
    fclose(fp);
    *pptr = s;
}

static void regular_check();

static void
reload_data()
{
    regular_check();

    if (!g_reload_data)
        return;

    fprintf(stderr, LOG_PREFIX "start reloading data.\r\n");
    g_reload_data = 0;
    g_welcome_mtime = dasht(FN_WELCOME);
    load_text_screen_file(FN_WELCOME, &welcome_screen);
    load_text_screen_file(FN_GOODBYE, &goodbye_screen);
    load_text_screen_file(FN_BAN,     &ban_screen);
}

static void
draw_text_screen(login_conn_ctx *conn, const char *scr)
{
    const char *ps, *pe;
    char buf[64];
    time4_t now;

    _mt_clear(conn);
    if (!scr || !*scr)
        return;

    // draw the screen from text file
    // XXX Because the text file may contain a very small subset of escape sequence
    // *t[Cdate] and *u[SHM->UTMPnumber], we implement a tiny version of 
    // expand_esc_star here.

    ps = pe = scr;
    while(pe && *pe)
    {
        // find a safe range between (ps, pe) to print
        pe = strchr(pe, ESC_CHR);
        if (!pe)
        {
            // no more escapes, print all.
            _buff_write(conn, ps, strlen(ps));
            break;
        }

        // let's look ahead
        pe++;

        // if not esc_star, search for next.
        if (*pe != '*')
            continue;

        // flush previous data
        _buff_write(conn, ps, pe - ps - 1);

        buf[0] = 0; pe++;

        // expand the star
        switch(*pe)
        {
            case 't':   // current time
                // strcpy(buf, "[date]");
                now = time(0);
                strlcpy(buf, Cdate(&now), sizeof(buf));
                break;

            case 'u':   // current online users
                // strcpy(buf, "[SHM->UTMPnumber]");
                snprintf(buf, sizeof(buf), "%d", SHM->UTMPnumber);
                break;
        }

        if(buf[0])
            _buff_write(conn, buf, strlen(buf));
        pe ++;
        ps = pe;
    }
}

static void
draw_goodbye(login_conn_ctx *conn)
{
    draw_text_screen(conn, goodbye_screen);
}

static void 
draw_userid_prompt(login_conn_ctx *conn, const char *uid, int icurr)
{
    char box[IDBOXLEN];

    _mt_move_yx(conn, LOGIN_PROMPT_YX);  _mt_clrtoeol(conn);
    _buff_write(conn, LOGIN_PROMPT_MSG, sizeof(LOGIN_PROMPT_MSG)-1);
    // draw input box
    memset(box, ' ', sizeof(box));
    if (uid) memcpy(box, uid, strlen(uid));
    _buff_write (conn, box,   sizeof(box));
    memset(box, '\b',sizeof(box));
    _buff_write (conn, box,   sizeof(box)-icurr);
}

static void
draw_userid_prompt_end(login_conn_ctx *conn)
{
    // if (g_verbose) fprintf(stderr, LOG_PREFIX "reset connection attribute.\r\n");
    _buff_write(conn, LOGIN_PROMPT_END, sizeof(LOGIN_PROMPT_END)-1);
}

static void
draw_passwd_prompt(login_conn_ctx *conn)
{
    _mt_move_yx(conn, PASSWD_PROMPT_YX); _mt_clrtoeol(conn);
    _buff_write(conn, PASSWD_PROMPT_MSG, sizeof(PASSWD_PROMPT_MSG)-1);
}

static void
draw_empty_userid_warn(login_conn_ctx *conn)
{
    _mt_move_yx(conn, USERID_EMPTY_YX); _mt_clrtoeol(conn);
    _buff_write(conn, USERID_EMPTY_MSG, sizeof(USERID_EMPTY_MSG)-1);
}

static void 
draw_check_passwd(login_conn_ctx *conn)
{
    _mt_move_yx(conn, PASSWD_CHECK_YX); _mt_clrtoeol(conn);
    _buff_write(conn, PASSWD_CHECK_MSG, sizeof(PASSWD_CHECK_MSG)-1);
}

static void
draw_auth_success(login_conn_ctx *conn, int free)
{
    if (free)
    {
        _mt_move_yx(conn, FREEAUTH_SUCCESS_YX); _mt_clrtoeol(conn);
        _buff_write(conn, FREEAUTH_SUCCESS_MSG, sizeof(FREEAUTH_SUCCESS_MSG)-1);
    } else {
        _mt_move_yx(conn, AUTH_SUCCESS_YX); _mt_clrtoeol(conn);
        _buff_write(conn, AUTH_SUCCESS_MSG, sizeof(AUTH_SUCCESS_MSG)-1);
    }
}

static void
draw_auth_fail(login_conn_ctx *conn)
{
    _mt_move_yx(conn, AUTH_FAIL_YX); _mt_clrtoeol(conn);
    _buff_write(conn, AUTH_FAIL_MSG, sizeof(AUTH_FAIL_MSG)-1);
}

static void
draw_service_failure(login_conn_ctx *conn)
{
    _mt_move_yx(conn, PASSWD_CHECK_YX); _mt_clrtoeol(conn);
    _mt_move_yx(conn, SERVICE_FAIL_YX); _mt_clrtoeol(conn);
    _buff_write(conn, SERVICE_FAIL_MSG, sizeof(SERVICE_FAIL_MSG)-1);
}

static void
draw_overload(login_conn_ctx *conn, int type)
{
    // XXX currently overload is displayed immediately after
    // banner/INSCREEN, so an enter is enough.
    _buff_write(conn, "\r\n", 2);
    // _mt_move_yx(conn, PASSWD_CHECK_YX); _mt_clrtoeol(conn);
    if (type == 1)
    {
        // _mt_move_yx(conn, OVERLOAD_CPU_YX); _mt_clrtoeol(conn);
        _buff_write(conn, OVERLOAD_CPU_MSG, sizeof(OVERLOAD_CPU_MSG)-1);
    } 
    else if (type == 2)
    {
        // _mt_move_yx(conn, OVERLOAD_USER_YX); _mt_clrtoeol(conn);
        _buff_write(conn, OVERLOAD_USER_MSG, sizeof(OVERLOAD_USER_MSG)-1);
    } 
    else {
        assert(false);
        // _mt_move_yx(conn, OVERLOAD_CPU_YX); _mt_clrtoeol(conn);
        _buff_write(conn, OVERLOAD_CPU_MSG, sizeof(OVERLOAD_CPU_MSG)-1);
    }
}

static void
draw_reject_free_userid(login_conn_ctx *conn, const char *freeid)
{
    _mt_move_yx(conn, PASSWD_CHECK_YX); _mt_clrtoeol(conn);
#ifdef STR_GUEST
    if (strcasecmp(freeid, STR_GUEST) == 0)
    {
        _mt_move_yx(conn, TOO_MANY_GUEST_YX); _mt_clrtoeol(conn);
        _buff_write(conn, TOO_MANY_GUEST_MSG, sizeof(TOO_MANY_GUEST_MSG)-1);
        return;
    }
#endif
    _mt_move_yx(conn, REJECT_FREE_UID_YX); _mt_clrtoeol(conn);
    _buff_write(conn, REJECT_FREE_UID_MSG, sizeof(REJECT_FREE_UID_MSG)-1);

}

///////////////////////////////////////////////////////////////////////
// BBS Logic

static void
regular_check()
{
    // cache results
    static time_t last_check_time = 0;
    time_t now = time(0);

    if ( now - last_check_time < LOGIND_REGULAR_CHECK_DURATION)
        return;

    last_check_time = now;
    g_overload = 0;
    g_banned   = 0;

#ifndef LOGIND_DONT_CHECK_FREE_USERID
    g_guest_too_many = 0;
    g_guest_usernum  = 0;
#endif

    if (cpuload(NULL) > MAX_CPULOAD)
    {
        g_overload = 1;
    }
    else if (SHM->UTMPnumber >= MAX_ACTIVE
#ifdef DYMAX_ACTIVE
            || (SHM->GV2.e.dymaxactive > 2000 &&
                SHM->UTMPnumber >= SHM->GV2.e.dymaxactive)
#endif
        )
    {
        ++SHM->GV2.e.toomanyusers;
        g_overload = 2;
    }

    if (dashf(FN_BAN))
    {
        g_banned = 1;
        load_text_screen_file(FN_BAN, &ban_screen);
    }

    // check welcome screen
    if (g_verbose) 
        fprintf(stderr, LOG_PREFIX "check welcome screen.\r\n");
    if (dasht(FN_WELCOME) != g_welcome_mtime)
    {
        g_reload_data = 1;
        if (g_verbose)
            fprintf(stderr, LOG_PREFIX 
                    "modified. must update welcome screen ...\r\n");
    }
}

static int 
check_banip(char *host)
{
    uint32_t thisip = ipstr2int(host);
    return uintbsearch(thisip, &banip[1], banip[0]) ? 1 : 0;
}

static const char *
auth_is_free_userid(const char *userid)
{
#if defined(STR_GUEST) && !defined(NO_GUEST_ACCOUNT_REG)
    if (strcasecmp(userid, STR_GUEST) == 0)
        return STR_GUEST;
#endif

#ifdef STR_REGNEW 
    if (strcasecmp(userid, STR_REGNEW) == 0)
        return STR_REGNEW;
#endif

    return NULL;
}
static int
auth_check_free_userid_allowance(const char *userid)
{
#ifdef LOGIND_DONT_CHECK_FREE_USERID
    // XXX experimental to disable free id checking
    return 1;
#endif

#ifdef STR_REGNEW
    // accept all 'new' command.
    if (strcasecmp(userid, STR_REGNEW) == 0)
        return 1;
#endif

#ifdef STR_GUEST
    if (strcasecmp(userid, STR_GUEST) == 0)
    {
#  ifndef MAX_GUEST
        g_guest_too_many = 0;
#  else
        // if already too many guest, fast reject until next regular check.
        // XXX TODO also cache if guest is not too many?
        if (g_guest_too_many)
            return 0;

        // now, load guest account information.
        if (!g_guest_usernum)
        {
            if (g_verbose) fprintf(stderr, LOG_PREFIX " reload guest information\r\n");

            // reload guest information
            g_guest_usernum = searchuser(STR_GUEST, NULL);

            if (g_guest_usernum < 1 || g_guest_usernum > MAX_USERS)
                g_guest_usernum = 0;

            // if guest is not created, it's administrator's problem...
            assert(g_guest_usernum);
        }

        // update the 'too many' status.
        g_guest_too_many = 
            (!g_guest_usernum || (search_ulistn(g_guest_usernum, MAX_GUEST) != NULL));

        if (g_verbose) fprintf(stderr, LOG_PREFIX " guests are %s\r\n",
                g_guest_too_many ? "TOO MANY" : "ok.");

#  endif // MAX_GUEST
        return g_guest_too_many ? 0 : 1;
    }
#endif // STR_GUEST

    // shall never reach here.
    assert(0);
    return 0;
}


// NOTE ctx->passwd will be destroyed (must > PASSLEN+1)
// NOTE ctx->userid may be changed (must > IDLEN+1)
static int
auth_user_challenge(login_ctx *ctx)
{
    char *uid = ctx->userid,
         *passbuf = ctx->passwd;
    const char *free_uid = auth_is_free_userid(uid);
    userec_t user = {0};

    if (free_uid)
    {
        strlcpy(ctx->userid, free_uid, sizeof(ctx->userid));
        return AUTH_RESULT_FREEID;
    }

    if (passwd_load_user(uid, &user) < 1 ||
        !user.userid[0] ||
        !checkpasswd(user.passwd, passbuf) )
    {
        if (user.userid[0])
            strcpy(uid, user.userid);
        return AUTH_RESULT_FAIL;
    }

    // normalize user id
    strcpy(uid, user.userid);
    return AUTH_RESULT_OK;
}

static void
retry_service()
{
    // empty g_tunnel means the service is not started or waiting retry.
    if (!g_tunnel)
        return ;

    g_tunnel = 0;

    // now, see if we can retry for it.
    if (!*g_retry_cmd)
        return;

    if (g_retry_times >= LOGIND_MAX_RETRY_SERVICE)
    {
        fprintf(stderr, LOG_PREFIX 
                "retry too many times (>%d), stop and wait manually maintainance.\r\n",
                LOGIND_MAX_RETRY_SERVICE);
        return;
    }

    g_retry_times++;
    fprintf(stderr, LOG_PREFIX "#%d retry to start service: %s\r\n", 
            g_retry_times, g_retry_cmd);
    system(g_retry_cmd);
}

static int
login_conn_end_ack(login_conn_ctx *conn, void *ack, int fd);

static int 
start_service(int fd, login_conn_ctx *conn)
{
    login_data ld = {0};
    int ack = 0;
    login_ctx *ctx;

    if (!g_tunnel)
        return 0;

    assert(conn);
    ctx = &conn->ctx;

    ld.cb  = sizeof(ld);
    ld.ack = (void*)conn;

    strlcpy(ld.userid, ctx->userid, sizeof(ld.userid));
    strlcpy(ld.hostip, ctx->hostip, sizeof(ld.hostip));
    strlcpy(ld.port,   ctx->port,   sizeof(ld.port));
    ld.encoding = ctx->encoding;
    ld.client_code = ctx->client_code;
    ld.t_lines  = 24;   // default size
    ld.t_cols   = 80;
    if (ctx->t_lines > ld.t_lines)
        ld.t_lines = ctx->t_lines;
    if (ctx->t_cols > ld.t_cols)
        ld.t_cols = ctx->t_cols;

    if (g_verbose) fprintf(stderr, LOG_PREFIX "start new service: %s@%s:%s #%d\r\n",
            ld.userid, ld.hostip, ld.port, fd);

    // XXX simulate the cache re-construction in mbbsd/login_query.
    resolve_garbage();

    // since mbbsd is running in blocking mode, let's re-configure fd.
    _disable_nonblock(fd);

    // deliver the fd to hosting service
    if (send_remote_fd(g_tunnel, fd) < 0)
    {
        if (g_verbose) fprintf(stderr, LOG_PREFIX
                "failed in send_remote_fd\r\n");
        return ack;
    }
   
    // deliver the login data to hosting servier
    if (towrite(g_tunnel, &ld, sizeof(ld)) < sizeof(ld))
    {
        if (g_verbose) fprintf(stderr, LOG_PREFIX
                "failed in towrite(login_data)\r\n");
        return ack;
    }

    // wait (or async) service to response
    if (!login_conn_end_ack(conn, ld.ack, fd))
    {
        if (g_verbose) fprintf(stderr, LOG_PREFIX
                "failed in logind_conn_end_ack\r\n");
        return ack;
    }
    return 1;
}

static int 
auth_start(int fd, login_conn_ctx *conn)
{
    login_ctx *ctx = &conn->ctx;
    int isfree = 0, was_valid_uid = 0;
    draw_check_passwd(conn);

    if (is_validuserid(ctx->userid))
    {
        // ctx content may be changed.
        was_valid_uid = 1;
        switch (auth_user_challenge(ctx))
        {
            case AUTH_RESULT_FAIL:
                logattempt(ctx->userid , '-', time(0), ctx->hostip);
                break;

            case AUTH_RESULT_FREEID:
                isfree = 1;
                // share FREEID case, no break here!
            case AUTH_RESULT_OK:
                if (!isfree)
                {
                    logattempt(ctx->userid , ' ', time(0), ctx->hostip);
                }
                else if (!auth_check_free_userid_allowance(ctx->userid))
                {
                    // XXX since the only case of free
                    draw_reject_free_userid(conn, ctx->userid);
                    return AUTH_RESULT_STOP;
                }

                draw_auth_success(conn, isfree);

                if (!start_service(fd, conn))
                {
                    // too bad, we can't start service.
                    retry_service();
                    draw_service_failure(conn);
                    return AUTH_RESULT_STOP;
                }
                return AUTH_RESULT_OK;

            default:
                assert(false);
                break;
        }

    }

    // auth fail.
    _mt_bell(conn);

    // if fail, restart
    if (login_ctx_retry(ctx) >= LOGINATTEMPTS)
    {
        // end retry.
        draw_goodbye(conn);
        if (g_verbose) fprintf(stderr, LOG_PREFIX "auth fail (goodbye):  %s@%s  #%d...",
                conn->ctx.userid, conn->ctx.hostip, fd);
        return AUTH_RESULT_STOP;

    }

    // prompt for retry
    if (was_valid_uid)
        draw_auth_fail(conn);
    else
        draw_empty_userid_warn(conn);

    ctx->state = LOGIN_STATE_USERID;
    draw_userid_prompt(conn, NULL, 0);
    return AUTH_RESULT_RETRY;
}

///////////////////////////////////////////////////////////////////////
// Event callbacks

static struct event ev_sighup, ev_tunnel, ev_ack;

static void 
sighup_cb(int signal, short event, void *arg)
{
    fprintf(stderr, LOG_PREFIX 
            "caught sighup (request to reload) with %u opening fd...\r\n",
            g_opened_fd);
    g_reload_data = 1;
}

static void
stop_g_tunnel()
{
    if (!g_tunnel)
        return;

    close(g_tunnel);
    if (g_async_ack) event_del(&ev_ack);
    g_tunnel = 0;
}

static void
stop_tunnel(int tunnel_fd)
{
    if (!tunnel_fd)
        return;
    if (tunnel_fd == g_tunnel)
        stop_g_tunnel();
    else
        close(tunnel_fd);
}

static void 
endconn_cb(int fd, short event, void *arg)
{
    login_conn_ctx *conn = (login_conn_ctx*) arg;
    if (g_verbose) fprintf(stderr, LOG_PREFIX
            "login_conn_remove: removed connection (%s@%s) #%d...",
            conn->ctx.userid, conn->ctx.hostip, fd);

    // remove from ack queue
    if (conn->ctx.state == LOGIN_STATE_WAITACK)
    {
        // it should be already inside.
        ackq_del(conn);
    }

    event_del(&conn->ev);
    bufferevent_free(conn->bufev);
    close(fd);
    g_opened_fd--;
    free(conn);
    if (g_verbose) fprintf(stderr, " done.\r\n");
}

static void
endconn_cb_buffer(struct bufferevent * evb, short event, void *arg)
{
    login_conn_ctx *conn = (login_conn_ctx*) arg;

    // "event" for bufferevent and normal event are different
    endconn_cb(EVENT_FD(&conn->ev), 0, arg);
}

static void 
login_conn_remove(login_conn_ctx *conn, int fd, int sleep_sec)
{
    assert(conn->cb == sizeof(login_conn_ctx));
    if (!sleep_sec)
    {
        endconn_cb(fd, EV_TIMEOUT, (void*) conn);
    } else {
        struct timeval tv = { sleep_sec, 0};
        event_del(&conn->ev);
        event_set(&conn->ev, fd, 0, endconn_cb, conn);
        event_add(&conn->ev, &tv);
        if (g_verbose) fprintf(stderr, LOG_PREFIX
                "login_conn_remove: stop conn #%d in %d seconds later.\r\n", 
                fd, sleep_sec);
    }
}

static void *
get_tunnel_ack(int tunnel)
{
    void *arg = NULL;

    if (toread(tunnel, &arg, sizeof(arg)) < sizeof(arg) ||
        !arg)
    {
        // sorry... broken, let's shutdown the tunnel.
        if (g_verbose)
            fprintf(stderr, LOG_PREFIX "get_tunnel_ack: tunnel (%d) is broken.\r\n", tunnel);

        stop_tunnel(tunnel);
        return arg;
    }

    return arg;

}

static void
ack_cb(int tunnel, short event, void *arg)
{
    login_conn_ctx *conn = NULL;

    assert(tunnel);
    if (!(event & EV_READ))
    {
        // not read event (closed? timeout?)
        if (g_verbose) fprintf(stderr, LOG_PREFIX 
                "warning: invalid ack event at tunnel %d.\r\n", tunnel);
        stop_tunnel(tunnel);
        return;
    }

    assert(sizeof(arg) == sizeof(conn));
    conn = (login_conn_ctx*) get_tunnel_ack(tunnel);
    if (!conn)
    {
        if (g_verbose) fprintf(stderr, LOG_PREFIX 
                "warning: invalid ack at tunnel %d.\r\n", tunnel);
        return;
    }

    // XXX success connection.
    if (ackq_del(conn))
    {
        // reset the state to prevent processing ackq again
        conn->ctx.state = LOGIN_STATE_AUTH;
        // this event is still in queue.
        login_conn_remove(conn, conn->telnet.fd, 0);
    } else {
        if  (g_verbose) fprintf(stderr, LOG_PREFIX 
                "got invalid ack connection: (%08lX).\r\n", (unsigned long) conn);
    }
}


static int
login_conn_end_ack(login_conn_ctx *conn, void *ack, int fd)
{
    // fprintf(stderr, LOG_PREFIX "login_conn_end_ack: enter.\r\n");

    if (g_async_ack)
    {
        // simply wait for ack_cb to complete
        // fprintf(stderr, LOG_PREFIX "login_conn_end_ack: async mode.\r\n");

        // mark as queued for waiting ack
        conn->ctx.state = LOGIN_STATE_WAITACK;
        ackq_add(conn);

        // set a safe timeout
        login_conn_remove(conn, fd, ACK_TIMEOUT_SEC);

    } else {
        // wait service to complete
        void *rack = NULL;

        // fprintf(stderr, LOG_PREFIX "login_conn_end_ack: sync mode.\r\n");
        if (!g_tunnel)
            return 0;

        rack = get_tunnel_ack(g_tunnel);
        if (!rack)
            return 0;

        if (rack != ack)
        {
            // critical error!
            fprintf(stderr, LOG_PREFIX 
                    "login_conn_end_ack: failed in ack value (%08lX != %08lX).\r\n",
                    (unsigned long)rack, (unsigned long)ack);

            stop_g_tunnel();
            return 0;
        }

        // safe to close.
        login_conn_remove(conn, fd, 0);
    }
    return 1;
}

static void 
client_cb(int fd, short event, void *arg)
{
    login_conn_ctx *conn = (login_conn_ctx*) arg;
    int len, r;
    unsigned char buf[64], ch, *s = buf;

    // for time-out, simply close connection.
    if (event & EV_TIMEOUT)
    {
        endconn_cb(fd, EV_TIMEOUT, (void*) conn);
        return;
    }

    // XXX will this happen?
    if (!(event & EV_READ))
    {
        assert(event & EV_READ);
        return;
    }

    if ( (len = read(fd, buf, sizeof(buf))) <= 0)
    {
        // case to continue:
        if ((len < 0) && (errno == EINTR || errno == EAGAIN))
            return;

        // len == 0: EOF
        // len <  0: any other error.

        // close connection
        login_conn_remove(conn, fd, 0);
        return;
    }

    len = telnet_process(&conn->telnet, buf, len);

    while (len > 0)
    {
        int c = _handle_term_keys((char**)&s, &len);

        // for zero, ignore.
        if (!c)
            continue;

        if (c == KEY_UNKNOWN)
        {
            // XXX for stupid clients always doing anti-idle, 
            // user will get beeps and have no idea what happened...
            // _mt_bell(conn);
            continue;
        }

        // deal with context
        switch ( login_ctx_handle(&conn->ctx, c) )
        {
            case LOGIN_HANDLE_WAIT:
                break;

            case LOGIN_HANDLE_BEEP:
                _mt_bell(conn);
                break;

            case LOGIN_HANDLE_BS:
                _mt_bs(conn);
                break;

            case LOGIN_HANDLE_REDRAW_USERID:
                if (g_verbose) fprintf(stderr, LOG_PREFIX
                        "redraw userid: id=[%s], icurr=%d\r\n",
                        conn->ctx.userid, conn->ctx.icurr);
                draw_userid_prompt(conn, conn->ctx.userid, conn->ctx.icurr);
                break;

            case LOGIN_HANDLE_OUTC:
                ch = c;
                _buff_write(conn, &ch, 1);
                break;

            case LOGIN_HANDLE_PROMPT_PASSWD:
                // because prompt would reverse attribute, reset here.
                draw_userid_prompt_end(conn);

#ifdef DETECT_CLIENT
                // stop detection
                conn->telnet.cc_arg = NULL;
#endif
                if (conn->ctx.userid[0])
                {
                    char *uid = conn->ctx.userid;
                    char *uid_lastc = uid + strlen(uid)-1;

                    draw_passwd_prompt(conn);
#ifdef CONVERT
                    // convert encoding if required
                    switch(*uid_lastc)
                    {
                        case '.':   // GB mode
                            conn->ctx.encoding = CONV_GB;
                            *uid_lastc = 0;
                            break;
                        case ',':   // UTF-8 mode
                            conn->ctx.encoding = CONV_UTF8;
                            *uid_lastc = 0;
                            break;
                    }
                    // force to eliminate the extra field.
                    // (backward behavior compatible)
                    uid[IDLEN] = 0;
#endif
                    // accounts except free_auth [guest / new]
                    // require passwd.
                    if (!auth_is_free_userid(uid))
                        break;
                }

                // force changing state
                conn->ctx.state = LOGIN_STATE_AUTH;
                // XXX share start auth, no break here.
            case LOGIN_HANDLE_START_AUTH:
                if ((r = auth_start(fd, conn)) != AUTH_RESULT_RETRY)
                {
                    // for AUTH_RESULT_OK, the connection is handled in
                    // login_conn_end_ack.
                    if (r != AUTH_RESULT_OK)
                        login_conn_remove(conn, fd, AUTHFAIL_SLEEP_SEC);
                    return;
                }
                break;
        }
    }
}

static void 
listen_cb(int lfd, short event, void *arg)
{
    int fd;
    struct sockaddr_in xsin = {0};
    struct timeval idle_tv = { IDLE_TIMEOUT_SEC, 0};
    socklen_t szxsin = sizeof(xsin);
    login_conn_ctx *conn;
    bind_event *pbindev = (bind_event*) arg;

    while ( (fd = accept(lfd, (struct sockaddr *)&xsin, &szxsin)) >= 0 ) {

        // XXX note: NONBLOCK is not always inherited (eg, not on Linux).
        // So we have to set blocking mode for client again here.
        if (g_nonblock) _enable_nonblock(fd);

        // fast draw banner (don't use buffered i/o - this banner is not really important.)
#ifdef INSCREEN
        write(fd, INSCREEN, sizeof(INSCREEN));
#endif

        if ((conn = malloc(sizeof(login_conn_ctx))) == NULL) {
            close(fd);
            return;
        }
        memset(conn, 0, sizeof(login_conn_ctx));
        conn->cb = sizeof(login_conn_ctx);

        if ((conn->bufev = bufferevent_new(fd, NULL, NULL, endconn_cb_buffer, conn)) == NULL) {
            free(conn);
            close(fd);
            return;
        }

        g_opened_fd ++;
        reload_data();
        login_ctx_init(&conn->ctx);

        // initialize telnet protocol
        telnet_ctx_init(&conn->telnet, &telnet_callback, fd);
        telnet_ctx_set_write_arg (&conn->telnet, (void*) conn); // use conn for buffered events
        telnet_ctx_set_resize_arg(&conn->telnet, (void*) &conn->ctx);
#ifdef DETECT_CLIENT
        telnet_ctx_set_cc_arg(&conn->telnet, (void*) &conn->ctx);
#endif
#ifdef LOGIND_OPENFD_IN_AYT
        telnet_ctx_set_ayt_arg(&conn->telnet, (void*) conn); // use conn for buffered events
#endif
        // better send after all parameters were set
        telnet_ctx_send_init_cmds(&conn->telnet);

        // get remote ip & local port info
        inet_ntop(AF_INET, &xsin.sin_addr, conn->ctx.hostip, sizeof(conn->ctx.hostip));
        snprintf(conn->ctx.port, sizeof(conn->ctx.port), "%u", pbindev->port); // ntohs(xsin.sin_port));

        if (g_verbose) fprintf(stderr, LOG_PREFIX
                "new connection: fd=#%d %s:%s (opened fd: %d)\r\n", 
                fd, conn->ctx.hostip, conn->ctx.port, g_opened_fd);

        // set events
        event_set(&conn->ev, fd, EV_READ|EV_PERSIST, client_cb, conn);
        event_add(&conn->ev, &idle_tv);

        // check ban here?  XXX can we directly use xsin.sin_addr instead of ASCII form?
        if (g_banned || check_banip(conn->ctx.hostip) )
        {
            // draw ban screen, if available. (for banip, this is empty).
            draw_text_screen (conn, ban_screen);
            login_conn_remove(conn, fd, BAN_SLEEP_SEC);
            return;
        }

        // XXX check system load here.
        if (g_overload)
        {
            draw_overload    (conn, g_overload);
            login_conn_remove(conn, fd, OVERLOAD_SLEEP_SEC);
            return;

        } else {
            draw_text_screen  (conn, welcome_screen);
            draw_userid_prompt(conn, NULL, 0);
        }

        // in blocking mode, we cannot wait accept to return error.
        if (!g_nonblock)
            break;
    }
}

static void 
tunnel_cb(int fd, short event, void *arg)
{
    int cfd;
    if ((cfd = accept(fd, NULL, NULL)) < 0 )
        return;

    // got new tunnel
    fprintf(stderr, LOG_PREFIX "new tunnel established.\r\n");
    _set_connection_opt(cfd);

    stop_g_tunnel();
    g_tunnel = cfd;

    if (g_async_ack)
    {
        event_set(&ev_ack, g_tunnel, EV_READ | EV_PERSIST, ack_cb, NULL);
        event_add(&ev_ack, NULL);
    }
}

///////////////////////////////////////////////////////////////////////
// Main 

static int 
bind_port(int port)
{
    char buf[STRLEN];
    int sfd;
    bind_event *pev = NULL;

    snprintf(buf, sizeof(buf), "*:%d", port);

    fprintf(stderr, LOG_PREFIX "binding to port: %d...", port);
    if ( (sfd = tobindex(buf, LOGIND_SOCKET_QLEN, _set_bind_opt, 1)) < 0 )
    {
        fprintf(stderr, LOG_PREFIX "cannot bind to port: %d. abort.\r\n", port);
        return -1;
    }
    pev = malloc  (sizeof(bind_event));
    memset(pev, 0, sizeof(bind_event));
    assert(pev);

    pev->port = port;
    event_set(&pev->ev, sfd, EV_READ | EV_PERSIST, listen_cb, pev);
    event_add(&pev->ev, NULL);
    fprintf(stderr,"ok. \r\n");
    return 0;
}

static int 
parse_bindports_conf(FILE *fp, 
        char *tunnel_path, int sz_tunnel_path,
        char *tclient_cmd, int sz_tclient_cmd
        )
{
    char buf [STRLEN*3], vprogram[STRLEN], vport[STRLEN], vtunnel[STRLEN];
    int bound_ports = 0;
    int iport = 0;

    // format: [ vprogram port ] or [ vprogram tunnel path ]
    while (fgets(buf, sizeof(buf), fp))
    {
        if (sscanf(buf, "%s%s", vprogram, vport) != 2)
            continue;
        if (strcmp(vprogram, MY_SVC_NAME) != 0)
            continue;

        if (strcmp(vport, "client") == 0)
        {
            // syntax: client command-line$
            if (*tclient_cmd)
            {
                fprintf(stderr, LOG_PREFIX
                        "warning: ignored configuration file due to specified client command: %s\r\n",
                        tclient_cmd);
                continue;
            }
            if (sscanf(buf, "%*s%*s%[^\n]", vtunnel) != 1 || !*vtunnel)
            {
                fprintf(stderr, LOG_PREFIX "incorrect tunnel client configuration. abort.\r\n");
                exit(1);
            }
            if (g_verbose) fprintf(stderr, "client: %s\r\n", vtunnel);
            strlcpy(tclient_cmd, vtunnel, sz_tclient_cmd);
            continue;
        }
        if (strcmp(vport, "client_retry") == 0)
        {
            // syntax: client command-line$
            if (*g_retry_cmd)
            {
                fprintf(stderr, LOG_PREFIX
                        "warning: ignored configuration file due to specified retry command: %s\r\n",
                        g_retry_cmd);
                continue;
            }
            if (sscanf(buf, "%*s%*s%[^\n]", vtunnel) != 1 || !*vtunnel)
            {
                fprintf(stderr, LOG_PREFIX "incorrect retry client configuration. abort.\r\n");
                exit(1);
            }
            if (g_verbose) fprintf(stderr, "client_retry: %s\r\n", vtunnel);
            strlcpy(g_retry_cmd, vtunnel, sizeof(g_retry_cmd));
            continue;
        }
        else if (strcmp(vport, "tunnel") == 0)
        {
            if (*tunnel_path)
            {
                fprintf(stderr, LOG_PREFIX
                        "warning: ignored configuration file due to specified tunnel: %s\r\n",
                        tunnel_path);
                continue;
            }
            if (sscanf(buf, "%*s%*s%s", vtunnel) != 1 || !*vtunnel)
            {
                fprintf(stderr, LOG_PREFIX "incorrect tunnel configuration. abort.\r\n");
                exit(1);
            }
            if (g_verbose) fprintf(stderr, "tunnel: %s\r\n", vtunnel);
            strlcpy(tunnel_path, vtunnel, sz_tunnel_path);
            continue;
        }

        iport = atoi(vport);
        if (!iport)
        {
            fprintf(stderr, LOG_PREFIX "warning: unknown settings: %s\r\n", buf);
            continue;
        }

        if (bind_port(iport) < 0)
        {
            fprintf(stderr, LOG_PREFIX "cannot bind to port: %d. abort.\r\n", iport);
            exit(3);
        }
        bound_ports++;
    }
    return bound_ports;
}

int 
main(int argc, char *argv[])
{
    int     ch, port = 0, bound_ports = 0, tfd, as_daemon = 1;
    FILE   *fp;
    char tunnel_path[PATHLEN] = "", tclient_cmd[PATHLEN] = "";
    const char *config_file = FN_CONF_BINDPORTS;
    const char *log_file = NULL;


    Signal(SIGPIPE, SIG_IGN);

    while ( (ch = getopt(argc, argv, "f:p:t:l:r:hvDdBbAa")) != -1 )
    {
        switch( ch ){
        case 'f':
            config_file = optarg;
            break;

        case 'l':
            log_file = optarg;
            break;

        case 'p':
            if (optarg) port = atoi(optarg);
            break;

        case 't':
            strlcpy(tunnel_path, optarg, sizeof(tunnel_path));
            break;

        case 'r':
            strlcpy(g_retry_cmd, optarg, sizeof(g_retry_cmd));
            break;

        case 'd':
            as_daemon = 1;
            break;

        case 'D':
            as_daemon = 0;
            break;

        case 'a':
            g_async_ack = 1;
            break;

        case 'A':
            g_async_ack = 0;
            break;

        case 'b':
            g_nonblock = 1;
            break;

        case 'B':
            g_nonblock = 0;
            break;

        case 'v':
            g_verbose++;
            break;

        case 'h':
        default:
            fprintf(stderr,
                    "usage: %s [-aAbBvdD] [-l log_file] [-f conf] [-p port] [-t tunnel] [-c client_command]\r\n", argv[0]);
            fprintf(stderr, 
                    "\t-v: provide verbose messages\r\n"
                    "\t-d: enter daemon mode%s\r\n"
                    "\t-D: do not enter daemon mode%s\r\n"
                    "\t-a: use asynchronous service ack%s\r\n"
                    "\t-A: do not use async service ack%s\r\n"
                    "\t-b: use non-blocking socket mode%s\r\n"
                    "\t-B: use blocking socket mode%s\r\n"
                    "\t-f: read configuration from file (default: %s)\r\n", 
                    as_daemon   ? " (default)" : "", !as_daemon   ? " (default)" : "",
                    g_async_ack ? " (default)" : "", !g_async_ack ? " (default)" : "",
                    g_nonblock  ? " (default)" : "", !g_nonblock  ? " (default)" : "",
                    BBSHOME "/" FN_CONF_BINDPORTS);
            fprintf(stderr, 
                    "\t-l: log meesages into log_file\r\n"
                    "\t-p: bind (listen) to specific port\r\n"
                    "\t-t: create tunnel in given path\r\n"
                    "\t-c: spawn a (tunnel) client after initialization\r\n"
                    "\t-r: the command to retry spawning clients\r\n"
                   );
            return 1;
        }
    }

    struct rlimit r = {.rlim_cur = LOGIND_MAX_FDS, .rlim_max = LOGIND_MAX_FDS};
    if (setrlimit(RLIMIT_NOFILE, &r) < 0)
    {
        fprintf(stderr, LOG_PREFIX "warning: cannot increase max fd to %u...\r\n", LOGIND_MAX_FDS);
    }

    chdir(BBSHOME);
    attach_SHM();

    reload_data();

    struct event_base *evb = event_init();

    // bind ports
    if (port && bind_port(port) < 0)
    {
        fprintf(stderr, LOG_PREFIX "cannot bind to port: %d. abort.\r\n", port);
        return 3;
    }
    if (port)
        bound_ports++;

    // bind from port list file
    if( NULL != (fp = fopen(config_file, "rt")) )
    {
        bound_ports += parse_bindports_conf(fp, 
                tunnel_path, sizeof(tunnel_path),
                tclient_cmd, sizeof(tclient_cmd));
        fclose(fp);
    }

    if (!bound_ports)
    {
        fprintf(stderr, LOG_PREFIX "error: no ports to bind. abort.\r\n");
        return 4;
    }
    if (!*tunnel_path)
    {
        fprintf(stderr, LOG_PREFIX "error: must assign one tunnel path. abort.\r\n");
        return 4;
    }

    /* Give up root privileges: no way back from here */
    setgid(BBSGID);
    setuid(BBSUID);

    // create tunnel
    fprintf(stderr, LOG_PREFIX "creating tunnel: %s...", tunnel_path);
    if ( (tfd = tobindex(tunnel_path, 1, _set_bind_opt, 1)) < 0)
    {
        fprintf(stderr, LOG_PREFIX "cannot create tunnel. abort.\r\n");
        return 2;
    }
    fprintf(stderr, "ok.\r\n");
    event_set(&ev_tunnel, tfd, EV_READ | EV_PERSIST, tunnel_cb, &ev_tunnel);
    event_add(&ev_tunnel, NULL);

    // daemonize!
    if (as_daemon)
    {
        fprintf(stderr, LOG_PREFIX "start daemonize\r\n");
        daemonize(BBSHOME "/run/logind.pid", log_file);

        // because many of the libraries used in this daemon (for example,
        // passwd / logging / ...) all assume cwd=BBSHOME,
        // let's workaround them.
        chdir(BBSHOME);
    }

    // Some event notification mechanisms don't work across forks (e.g. kqueue)
    event_reinit(evb);

    // SIGHUP handler is reset in daemonize()
    signal_set(&ev_sighup, SIGHUP, sighup_cb, &ev_sighup);
    signal_add(&ev_sighup, NULL);

    // spawn tunnel client if specified.
    if (*tclient_cmd)
    {
        int r;
        fprintf(stderr, LOG_PREFIX "invoking client...\r\n");
        // XXX this should NOT be a blocking call.
        r = system(tclient_cmd);
        if (g_verbose)
            fprintf(stderr, LOG_PREFIX "client return value = %d\r\n", r);
    }

    // warning: after daemonize, the directory was changed to root (/)...
    fprintf(stderr, LOG_PREFIX "start event dispatch.\r\n");
    event_dispatch();

    return 0;
}

// vim:et
