/*
 * proxylib.h - proxy helper function declarations
 *
 * Author: Tian Xin
 * Andrew ID: txin
 */

/* error helpers */
void gai_error_non_exit(int code, char *msg);
void unix_error_non_exit(char *msg);
void posix_error_non_exit(int code, char *msg);

/* rio wrappers */
ssize_t proxy_rio_readnb(rio_t *rp, void *usrbuf, size_t n);
ssize_t proxy_rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen);
int proxy_rio_writen(int fd, void *usrbuf, size_t n);

/* client error response functions */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg);
void internal_server_error(int fd);

