#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>

#define BUF_SIZE 4096

int open_a6(char *name)
{
	int fd;
	fd = open(name, O_RDWR);
	if (fd == -1) {
		perror("fail");
		exit(1);
	}
	struct termios opt;
	tcgetattr(fd, &opt);
	cfsetispeed(&opt, B115200);
	cfsetospeed(&opt, B115200);
	opt.c_cflag |= CS8;
	opt.c_cflag &= ~CSTOPB;
	opt.c_cflag &= ~PARENB;

	opt.c_oflag &= ~(OPOST);
	opt.c_lflag &= ~(ISIG|ECHO|IEXTEN);  
	opt.c_iflag &= ~(INPCK|BRKINT|ICRNL|ISTRIP|IXON |INLCR);

	cfmakeraw(&opt);

	if (tcsetattr(fd, TCSAFLUSH, &opt) < 0) {
		printf("bad\n");
		exit(1);
	}

	return fd;
}

int get(int fd, char *buf)
{
	int ret = read(fd, buf, BUF_SIZE);
	if (ret < 0) {
		perror("get");
		exit(1);
	}
	return ret;
}

void get_n(int fd, char *buf, int len)
{
	int left, ret;
	left = len;
	while (left > 0) {
		int ret = read(fd, buf+(len-left), left);
		if (ret <= 0) {
			perror("get_n");
			exit(1);
		}
		left -= ret;
	}
}

int get_line(int fd, char *buf)
{
	int i;
	char c;
	for (i = 0; ; i++) {
		get_n(fd, &c, 1);
		if (c == '\n') {
			buf[i] = '\n';
			break;
		} else
			buf[i] = c;
	}
	i++;
	buf[i] = 0;
	return i;
}

void dump(char *ps, char *buf, int len)
{
#if 1
	int i;
	fprintf(stderr, "%s: ", ps);
#if 0
	for(i = 0; i < len; i++)
		fprintf(stderr, "%02x ", (unsigned)buf[i]);
	fprintf(stderr, "\n");
#endif
	if (len > 8)
		len = 8;
	write(STDERR_FILENO, buf, len);
	write(STDERR_FILENO, "\n", 1);
#endif
}

void put_n(int fd, char *buf, int len)
{
	int left, ret;
	left = len;
	while (left > 0) {
		int ret = write(fd, buf+(len-left), left);
		if (ret <= 0) {
			perror("put_n");
			exit(1);
		}
		left -= ret;
	}
}

int put_line(int fd, char *buf)
{
	int n = strlen(buf);
	put_n(fd, buf, n);
	return n;
}

int put_linev(int fd, char *fmt, ...)
{
	char buf[BUF_SIZE];
	va_list args;
	va_start(args, fmt);
	vsprintf(buf, fmt, args);
	va_end(args);
	return put_line(fd, buf);
}


struct ctx {
	int fd, peer_rfd, peer_wfd;
	enum { FREE, BUSY } state;
	enum { CLOSE, OPEN } conn_state;
	int slen;
	char sbuf[BUF_SIZE];
};

struct ctx *ctx_new(int fd, int peer_rfd, int peer_wfd)
{
	struct ctx *p = malloc(sizeof(struct ctx));
	p->fd = fd;
	p->peer_rfd = peer_rfd;
	p->peer_wfd = peer_wfd;
	p->state = FREE;
	p->conn_state = CLOSE;
	p->slen = 0;
	return p;
}

void a6_wait(struct ctx *ctx);

void on_peer_in(struct ctx *ctx)
{
	assert(!(ctx->state == BUSY || ctx->conn_state == CLOSE || ctx->slen));

	ctx->slen = get(ctx->peer_rfd, ctx->sbuf);

	if (ctx->slen) {
		put_linev(ctx->fd, "AT+CIPSEND=%d\r\n", ctx->slen);
		ctx->state = BUSY;
	} else {
		put_line(ctx->fd, "AT+CIPCLOSE\r\n");
		ctx->state = BUSY;
		a6_wait(ctx);
		exit(0);
	}
}

int on_a6_in(struct ctx *ctx)
{
	char buf[BUF_SIZE];

	get_n(ctx->fd, buf, 1);

	if (buf[0] == 0)
		return 0;

	if (buf[0] == '>') {
		get_n(ctx->fd, buf + 1, 1);
		put_n(ctx->fd, ctx->sbuf, ctx->slen);
		ctx->slen = 0;
		return 0;
	}

	int len = get_line(ctx->fd, buf + 1) + 1;

	dump("a6 in", buf, len);
	if (memcmp(buf, "OK", 2) == 0) {
		ctx->state = FREE;
	} else if (memcmp(buf, "CONNECT OK", 10) == 0) {
		ctx->conn_state = OPEN;
	} else if (memcmp(buf, "+CIPRCV:", 8) == 0) {
		char *p;
		int blen = strtol(buf + 8, &p, 10);
		assert(*p == ',');
		p++;
		int first_blen = len - (p - buf);

		if (first_blen >= blen)
			put_n(ctx->peer_wfd, p, blen);
		else {
			put_n(ctx->peer_wfd, p, first_blen);
			get_n(ctx->fd, buf, blen - first_blen);
			put_n(ctx->peer_wfd, buf, blen - first_blen);
		}
	} else if (memcmp(buf, "+TCPCLOSED", 10) == 0) {
		ctx->conn_state = CLOSE;
		ctx->state = FREE;
		exit(0);
	} else if (memcmp(buf, "+CME ERROR", 10) == 0 ||
		   memcmp(buf, "COMMAND NO RESPONSE", 19) == 0) {
		ctx->state = FREE;
		return 1;
	}
	return 0;
}

void a6_wait(struct ctx *ctx)
{
	while (ctx->state == BUSY)
		on_a6_in(ctx);
}

void on_timeout(struct ctx *ctx)
{
	if (ctx->state == BUSY || ctx->conn_state == CLOSE) {
		put_line(ctx->fd, "AT+CIPCLOSE\r\n");
		fprintf(stderr, "connect fail\n");
		exit(1);
	}
}

int main_loop(struct ctx *ctx)
{
	char buf[BUF_SIZE];
	int ret;

	int nfds;
	fd_set rfds;
	struct timeval tv;

	nfds = (ctx->peer_rfd > ctx->peer_wfd ? ctx->peer_rfd : ctx->peer_wfd);
	nfds = (ctx->fd > nfds ? ctx->fd : nfds);
	nfds++;

	while (1) {
		FD_ZERO(&rfds);
		FD_SET(ctx->fd, &rfds);

		if (!(ctx->state == BUSY || ctx->conn_state == CLOSE || ctx->slen)) {
			FD_SET(ctx->peer_rfd, &rfds);
		}

		tv.tv_sec = 10;
		tv.tv_usec = 0;
		ret = select(nfds, &rfds, NULL, NULL, &tv);
		if(ret == -1) {
			perror("select");
			exit(1);
		} else if(ret) {
			if(FD_ISSET(ctx->peer_rfd, &rfds))
				on_peer_in(ctx);
			if(FD_ISSET(ctx->fd, &rfds))
				if(on_a6_in(ctx))
					exit(1);
		} else
			on_timeout(ctx);
	}
	return 0;
}

int main()
{
	int fd = open_a6("/dev/ttyS1");
	int peer_rfd = STDIN_FILENO;
	int peer_wfd = STDOUT_FILENO;
	struct ctx *ctx = ctx_new(fd, peer_rfd, peer_wfd);

	put_line(ctx->fd, "ATE0\r\n");
	ctx->state = BUSY;
	a6_wait(ctx);

	put_line(ctx->fd, "AT+CIPCLOSE\r\n");
	ctx->conn_state = CLOSE;
	ctx->state = BUSY;
	a6_wait(ctx);

	put_line(ctx->fd, "AT+CIPSTART=\"TCP\",\"58.211.27.18\",22\r\n");
	ctx->state = BUSY;
	a6_wait(ctx);

	return main_loop(ctx);
}
