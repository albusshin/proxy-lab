/* 
 * proxy - A tiny HTTP proxy supporting only GET requests
 *
 * Author: Tian Xin
 * Andrew ID: txin
 * 
 * This implementation of proxy utilizes Linux network socket
 * interfaces for network connections, Robust I/O library for
 * file I/O, pthread library for concurrency and semaphores
 * for threads synchronization.
 */

#include <stdio.h>
#include "csapp.h"
//TODO decouple cache into cache.h and cache.c, Makefile

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define HTTP_PROTOCOL "http://"
#define HTTP_PROTOCOL_LEN 7
#define PORT_NUM_MIN 0
#define PORT_NUM_MAX 65535
#define DEFAULT_HTTP_PORT_STR "80"

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
/* Connection header value */
static const char *connection_hdr = "Connection: close\r\n";
/* Proxy-connection header value */
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";

/* the request information in proxy */
typedef struct proxy_info_type {
    int fd;
    char *hostname;
    char *port;
    char *uri;
    rio_t *p_server_rio;
} ProxyInfo;

/* the cache object linked list node */
typedef struct cache_node_type {
    char *absolute_uri;
    char *content;
    size_t size;
    time_t timestamp;
    struct cache_node_type *next;
    struct cache_node_type *prev;
} CacheNode;

//extern CacheNode *cache_head;       /* the cache linked list head */
//extern size_t cache_size;           /* the current cache size */
//extern sem_t reader_count_mutex;    /* the reader_count lock */
//extern sem_t writer_mutex;          /* the writer semaphore */
//extern int reader_count;            /* the current reader count */
CacheNode *cache_head;       /* the cache linked list head */
size_t cache_size = 0;      /* the current cache size */
sem_t reader_count_mutex;   /* the reader_count lock */
sem_t writer_mutex;         /* the writer semaphore */
int reader_count = 0;       /* the current reader count */

/* function declarations */

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

/* proxy core functions */
void serve_proxy(ProxyInfo *proxy_info);
void parse_uri(char *request_uri, char *hostname, char *port, char *uri);
void doit(int fd);
void *handle_request_thread(void *p_fd);

/* cache functions */
CacheNode *find_cache_node(char *absolute_uri);
void delete_cache_node(CacheNode *cache_node);
void evict_cache();
CacheNode *get_cache(char *absolute_uri);
void put_cache(char *absolute_uri, char *content, size_t size);

/* signal handler */
void sigpipe_handler(int sig);

/* end function declarations */


/* main - the main routine for the proxy */
int
main(int argc, char **argv) {
    int listenfd;
    int *connfdp;
    char hostname[MAXLINE], port[MAXLINE];
    int rc;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    /* set reader_count_mutex = 1 */
    V(&reader_count_mutex);
    /* set writer_mutex = 1 */
    V(&writer_mutex);
    pthread_t tid;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    /* Install the SIGPIPE signal handler */
    Signal(SIGPIPE,  sigpipe_handler);

    /* if listenfd cannot be opened on specified port, quit the proxy */
    if ((listenfd = open_listenfd(argv[1])) < 0) {
        fprintf(stderr, "Could not bind to port %s", argv[1]);
        exit(-1);
    }

    /* the main loop */
    while (1) {
	    clientlen = sizeof(clientaddr);
        /* get size of client addr */
        if ((connfdp = (int *) malloc(sizeof(int))) == NULL) {
            /* Try next iteration */
            unix_error_non_exit("malloc error");
            continue;
        }

        if ((*connfdp = accept(listenfd, (SA *)&clientaddr, &clientlen))
                < 0) {
            /* Try next connection */
            unix_error_non_exit("accept error");
            continue;
        }

        if ((rc = getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE,
                    port, MAXLINE, 0)) != 0) {
            gai_error_non_exit(rc, "getnameinfo error");
            /* If close connection fd failure again, exit */
            if (close(*connfdp) < 0)  {
                fprintf(stderr, "close failure\n");
            }
            free(connfdp);
            continue;
        }
        printf("Accepted from %s:%s\n", hostname, port);

        if ((rc = pthread_create(
                        &tid, NULL, handle_request_thread, connfdp)) != 0) {
	        posix_error_non_exit(rc, "pthread_create error");
            /* If close connection fd failure again, exit */
            if (close(*connfdp) < 0) {
                fprintf(stderr, "close failure\n");
            }
            free(connfdp);
        }
    }
    return 0;
}

/* serve_proxy - serve requested content as a proxy */
void serve_proxy(ProxyInfo *proxy_info) {
    rio_t rio;                  /* the rio as a client */
    int clientfd;               /* client descriptor */
    char buf[MAXLINE];          /* a buffer for reading and writing */
    char key_val_buf[MAXLINE];  /* a buffer to extract keys and values
                                   from request headers */

    /* take variables from struct */
    int fd = proxy_info -> fd;
    char *hostname = proxy_info -> hostname;
    char *port = proxy_info -> port;
    char *uri = proxy_info -> uri;
    rio_t *p_server_rio = proxy_info -> p_server_rio;

    char *cache_absolute_uri;
    if ((cache_absolute_uri = (char *) malloc(MAXLINE)) == NULL) {
        /* if malloc failure, ignore this request, keep calm and carry on */
        internal_server_error(fd);
        return;
    }
    /* construct the cache_absolute_uri */
    snprintf(cache_absolute_uri, MAXLINE, "%s:%s%s", hostname, port, uri);

    CacheNode *cache_node;

    if ((cache_node = get_cache(cache_absolute_uri)) != NULL) {
        /* cache hit, return the result directly */
        printf("Cache hit, time: %lu\n", 
                (unsigned long) cache_node -> timestamp);
        proxy_rio_writen(fd, cache_node -> content, cache_node -> size);
        return;
    }

    /* Try to open cilentfd and connect to requested web server */
    if ((clientfd = open_clientfd(hostname, port)) < 0) {
        internal_server_error(fd);
        return;
    }

    Rio_readinitb(&rio, clientfd);

    /* trasmit first line of request */
    snprintf(buf, MAXLINE, "GET %s HTTP/1.0\r\n", uri);
    proxy_rio_writen(clientfd, buf, strlen(buf));

    /* read request headers */
    int host_set = 0;
    while(strcmp(buf, "\r\n")) {
	    proxy_rio_readlineb(p_server_rio, buf, MAXLINE);
        strncpy(key_val_buf, buf, MAXLINE);
        char *p_split;
        if ((p_split = strstr(key_val_buf, ":")) != NULL) {
            *p_split = '\0';
            char *keybuf = key_val_buf;

            if (!strcasecmp("Host", keybuf)) {
                host_set = 1;
            }
            if (!strcasecmp("User-Agent", keybuf)) {
                /* ignore user-agent header */
                continue;
            }
            if (!strcasecmp("Connection", keybuf)) {
                /* ignore connection header */
                continue;
            }
            if (!strcasecmp("Proxy-Connection", keybuf)) {
                /* ignore proxy-connection header */
                continue;
            }
        }
        /* write client request to server */
        proxy_rio_writen(clientfd, buf, strlen(buf));
    }
    /* write host-header */
    if (!host_set) {
        snprintf(buf, MAXLINE, "Host: %s", hostname);
        proxy_rio_writen(clientfd, buf, strlen(buf));
    }
    /* write user-agent header */
    proxy_rio_writen(clientfd, (char *) user_agent_hdr, strlen(user_agent_hdr));
    /* write connection header */
    proxy_rio_writen(clientfd, (char *) connection_hdr, strlen(connection_hdr));
    /* write proxy-connection header */
    proxy_rio_writen(clientfd, (char *) proxy_connection_hdr, strlen(proxy_connection_hdr));

    char *cache_content;
    if ((cache_content = (char *) malloc(MAX_OBJECT_SIZE)) == NULL) {
        /* if malloc failure, ignore this request and carry on */
        internal_server_error(fd);
        if (close(clientfd) < 0) {
            fprintf(stderr, "close failure\n");
        }
        return;
    }
    char *cachep = cache_content;       /* memcpy pointer */
    size_t s = 0;                       /* read size */
    size_t cache_object_size = 0;       /* object size */
    while ((s = proxy_rio_readnb(&rio, buf, MAXLINE)) && s != -1) {
        if (proxy_rio_writen(fd, buf, s) == -1) {
            break;
        }
        cache_object_size += s;
        if (cache_object_size <= MAX_OBJECT_SIZE) {
            memcpy(cachep, buf, s);
            cachep += s;
        }
    }
    if (cache_object_size <= MAX_OBJECT_SIZE) {
        /* put cache object into cache only if its size is small enough */
        put_cache(cache_absolute_uri, cache_content, cache_object_size);
    }
    if (close(clientfd) < 0) {
        fprintf(stderr, "close failure\n");
    }
}

/*
 * parse_uri - parse the request uri into hostname, port and the relative uri
 */
void parse_uri(char *request_uri, char *hostname, char *port, char *uri) {
    char *p;
    /* Parse hostname */
    strncpy(hostname, request_uri, MAXLINE);
    if (strstr(hostname, HTTP_PROTOCOL)) {
        /* truncate leading "http://" */
        strncpy(hostname, hostname + HTTP_PROTOCOL_LEN, MAXLINE);
    }
    if ((p = strstr(hostname, ":")) != NULL) {
        /* truncate port */
        *p = '\0';
    }
    if ((p = strstr(hostname, "/")) != NULL) {
        /* truncate uri after '/' */
        *p = '\0';
    }

    /* Parse port number */
    strncpy(port, request_uri, MAXLINE);
    if (strstr(port, HTTP_PROTOCOL)) {
        /* truncate leading "http://" */
        strncpy(port, port + HTTP_PROTOCOL_LEN, MAXLINE);
    }
    if ((p = strstr(port, "/")) != NULL) {
        /* truncate uri after '/' */
        *p = '\0';
    }

    if ((p = strstr(port, ":")) != NULL) {
        /* truncate port */
        strncpy(port, p + 1, MAXLINE);
        /* convert string port number to int, truncating illegal chars */
        int port_num = atoi(port);
        if (port_num < PORT_NUM_MIN || port_num > PORT_NUM_MAX) {
            strncpy(port, DEFAULT_HTTP_PORT_STR, MAXLINE);
        }
    }
    else {
        /* If there is no port found, specify port num as 80 */
        strncpy(port, DEFAULT_HTTP_PORT_STR, MAXLINE);
    }

    /* Parse uri */
    strncpy(uri, request_uri, MAXLINE);
    if (strstr(uri, HTTP_PROTOCOL)) {
        /* truncate leading "http://" */
        strncpy(uri, uri + HTTP_PROTOCOL_LEN, MAXLINE);
    }
    if ((p = strstr(uri, "/")) != NULL) {
        /* get uri after '/' */
        uri = strncpy(uri, p, MAXLINE);
    }
    else {
        /* if no uri is set in the request, set the uri as "/" (the root) */
        strncpy(uri, "/", MAXLINE);
    }
}

/*
 * doit - handle an HTTP GET request in the proxy 
 */
void doit(int fd) {
    char buf[MAXLINE];          /* the buffer for reading and writing */
    char method[MAXLINE];       /* the request method */
    char request_uri[MAXLINE];  /* the request uri */
    char version[MAXLINE];      /* the request http version */

    char hostname[MAXLINE];     /* the requested server hostname */
    char port[MAXLINE];         /* the requested server port */
    char uri[MAXLINE];          /* the requested resource uri */
    rio_t server_rio;           /* the rio object for the request client */

    Rio_readinitb(&server_rio, fd);
    /* read the first line of request and parse it */
    if (!proxy_rio_readlineb(&server_rio, buf, MAXLINE))
        return;

    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, request_uri, version);
    /* check if is GET method */
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not Implemented",
                    "This proxy does not implement this method");
        printf("Rejected method %s\n", method);
        return;
    }
    /* check if the protocol is http */
    if (strstr(request_uri, HTTP_PROTOCOL) != request_uri) {
        clienterror(fd, request_uri, "400", "Bad Request",
                    "Request URI does not lead with \"http://\".");
        printf("Rejected URI %s\n", request_uri);
        return;
    }
    /* check if http version is HTTP/1.0 or HTTP/1.1 */
    if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) {
        clienterror(fd, version, "501", "Not Implemented",
                    "This HTTP version is not supported.");
        printf("Rejected version %s\n", version);
    }
    /* parse the hostname, port and uri from request_uri */
    parse_uri(request_uri, hostname, port, uri);

    /* using getaddrinfo to get the validity of hostname and port */
    struct addrinfo hints, *listp;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;  /* Open a connection */
    hints.ai_flags = AI_NUMERICSERV;  /* ... using a numeric port arg. */
    hints.ai_flags |= AI_ADDRCONFIG;  /* Recommended for connections */
    int rc;
    if ((rc = getaddrinfo(hostname, port, &hints, &listp)) != 0) {
        /* if something's wrong with getaddrinfo, the request is bad. */
        Freeaddrinfo(listp);
        char cause[MAXLINE];
        snprintf(cause, MAXLINE, "hostname: %s, port: %s", hostname, port);
        clienterror(fd, cause, "400", "Bad Request",
                    "Malformed hostname or port number.");
        gai_error_non_exit(rc, "Getaddrinfo error");
	    if (close(fd) < 0){
            fprintf(stderr, "close failure\n");
        }
        return;
    }

    /* construct the proxy_info struct and call serve_proxy to serve page */
    ProxyInfo proxy_info;
    proxy_info.fd = fd;
    proxy_info.hostname = hostname;
    proxy_info.port = port;
    proxy_info.uri = uri;
    proxy_info.p_server_rio = &server_rio;

	serve_proxy(&proxy_info);

    /* close the proxy fd */
	if (close(fd) < 0){
        fprintf(stderr, "close failure\n");
    }
}

/*
 * clienterror - respond with an error message
 *      to the client identified by fd
 */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    snprintf(body, MAXBUF, "<html><title>Proxy Error</title>");
    snprintf(body, MAXBUF, "%s<body bgcolor=""ffffff"">\r\n", body);
    snprintf(body, MAXBUF, "%s%s: %s\r\n", body, errnum, shortmsg);
    snprintf(body, MAXBUF, "%s<p>%s: %s\r\n", body, longmsg, cause);
    snprintf(body, MAXBUF, "%s<hr><em>The Proxy Server</em>\r\n", body);

    /* Print the HTTP response */
    snprintf(buf, MAXBUF, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    proxy_rio_writen(fd, buf, strlen(buf));
    snprintf(buf, MAXBUF, "Content-type: text/html\r\n");
    proxy_rio_writen(fd, buf, strlen(buf));
    snprintf(buf, MAXBUF, "Content-length: %d\r\n\r\n", (int)strlen(body));
    proxy_rio_writen(fd, buf, strlen(buf));
    proxy_rio_writen(fd, body, strlen(body));
}

/* 
 * sigpipe_handler - The kernel sends a SIGPIPE signal to a process
 * that has a handle to a socket which has been broken.
 * outputs a logging message and do do nothing.
 */
void 
sigpipe_handler(int sig) 
{
    /* Saving previous errno */
    int olderrno = errno;
    sio_puts("sigpipe_handler called.\n");
    /* Restoring previous errno */
    errno = olderrno;
}

/*
 * gai_error_non_exit -
 *      prints getaddrinfo style error message without exiting the program
 */
void gai_error_non_exit(int code, char *msg) {
    fprintf(stderr, "%s: %s\n", msg, gai_strerror(code));
}

/*
 * unix_error_non_exit -
 *      prints unix style error message without exiting the program
 */
void unix_error_non_exit(char *msg) {
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
}

/*
 * posix_error_non_exit -
 *      prints posix style error message without exiting the program
 */
void posix_error_non_exit(int code, char *msg) {
    fprintf(stderr, "%s: %s\n", msg, strerror(code));
}

/*
 * proxy_rio_readnb -
 *      rio_readnb wrapper.
 *      On failure, if errno is set to ECONNRESET, log the error.
 *      otherwise, exit the program with an error message.
 */
ssize_t proxy_rio_readnb(rio_t *rp, void *usrbuf, size_t n) {
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0) {
        if (errno == ECONNRESET) {
            unix_error_non_exit("proxy_rio_readnb error");
        }
        else {
	        internal_server_error(rp -> rio_fd);
        }
    }
    return rc;
}

/*
 * proxy_rio_readlineb -
 *      rio_readlineb wrapper.
 *      On failure, if errno is set to ECONNRESET, log the error.
 *      otherwise, exit the program with an error message.
 */
ssize_t proxy_rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen) {
    ssize_t rc;

    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0) {
        if (errno == ECONNRESET) {
            unix_error_non_exit("proxy_rio_readnb error");
        }
        else {
	        internal_server_error(rp -> rio_fd);
        }
    }
    return rc;
} 

/*
 * proxy_rio_writen -
 *      rio_writen wrapper.
 *      On failure, if errno is set to ECONNRESET or EPIPE, log the error.
 *      otherwise, exit the program with an error message.
 */
int proxy_rio_writen(int fd, void *usrbuf, size_t n) {
    if (rio_writen(fd, usrbuf, n) != n) {
        if (errno == EPIPE || errno == ECONNRESET) {
            unix_error_non_exit("proxy_rio_writen error");
        }
        else {
	        unix_error("proxy_rio_writen error");
        }
        return -1;
    }
    return n;
}

/*
 * internal_server_error -
 *      respond the client with an internal server error message.
 */
void internal_server_error(int fd) {
    clienterror(fd, "", "500", "Internal Server Error",
                "The proxy server encountered a problem");
}


/*
 * handle_request_thread - the thread function
 */
void *handle_request_thread(void *p_fd) {
    /* change the thread into detached state */
    /* if thread cannot detatch or identify self,
     * cannot proceed with normal workflow.
     * exit with failure message. */
    Pthread_detach(Pthread_self());
    /* call  */
    doit(*((int *) p_fd));
    free(p_fd);
    return NULL;
}

/*
 * find_cache_node - 
 *      a helper to find the cache object node
 *      inside the cache linked list with absolute_uri
 */
CacheNode *find_cache_node(char *absolute_uri) {
    CacheNode *p = cache_head;
    while (p) {
        if (!strncmp(absolute_uri, p -> absolute_uri, MAXLINE)) {
            return p;
        }
        p = p -> next;
    }
    return NULL;
}

/*
 * delete_cache_node -
 *      a helper to delete the cache object node from the cache linked list
 */
void delete_cache_node(CacheNode *cache_node) {
    if (cache_node == cache_head) {
        cache_head = cache_head -> next;
    }
    else {
        cache_node -> prev -> next = cache_node -> next;
    }
    if (cache_node -> next) {
        cache_node -> next -> prev = cache_node -> prev;
    }
    free(cache_node -> absolute_uri);
    free(cache_node -> content);
    free(cache_node);
}

/*
 * evict_cache -
 *      cache evict method
 *      Delete the oldest cache object node from the cache.
 */
void evict_cache() {
    CacheNode *p, *to_evict = cache_head;
    /* find the oldest cache object node */
    for (p = cache_head; p; p = p -> next) {
        /* Because the timestamp is measured in seconds, there might be
         * many nodes with the same timestamps. We want to evict the oldest
         * object, and because when putting into cache we put the new cache
         * object node in the head of the linked list, the rightmost node with 
         * the same timestamp value is the oldest one */
        if (p -> timestamp <= to_evict -> timestamp) {
            to_evict = p;
        }
    }
    /* log to console about eviction */
    printf("Cache evict, timestamp:%lu\n", 
            (unsigned long) to_evict -> timestamp);
    /* deduct the current cache size */
    /* only writer can do evictions, and only one writer can write */
    /* so no need to lock the cache_size variable */
    cache_size -= to_evict -> size;
    /* delete the cache object node from the linked list */
    delete_cache_node(to_evict);
}

/*
 * get_cache - 
 *      cache get method
 *      Read the cache object with the provided absolute_uri.
 */
CacheNode *get_cache(char *absolute_uri) {
    /* lock before updating reader_count */
    P(&reader_count_mutex);
    reader_count++;
    if (reader_count == 1) { /* First reader in, lock writers */
        P(&writer_mutex);
    }
    V(&reader_count_mutex);

    CacheNode * ret = find_cache_node(absolute_uri);
    if (ret) {
        /* updating the last used timestamp on the cache object */
        ret -> timestamp = time(NULL);
    }

    /* lock before updating reader_count */
    P(&reader_count_mutex);
    reader_count--;
    if (reader_count == 0) { /* Last reader out, unlock writers */
        V(&writer_mutex);
    }
    V(&reader_count_mutex);
    return ret;
}

/*
 * put_cache -
 *      cache put method
 *      Write a new cache object with the provided information.
 *      If the cache is full, evict cache nodes until the room is
 *      large enough to store the new cache object node.
 */
void put_cache(char *absolute_uri, char *content, size_t size) {
    /* acquire writer lock */
    P(&writer_mutex);
    cache_size += size;
    /* if total size is larger than the max cache size,
     * do cache evictions until this object can be stored in the cache */
    while (cache_size > MAX_CACHE_SIZE) {
        evict_cache();
    }
    CacheNode *cache_node;
    if ((cache_node = find_cache_node(absolute_uri)) != NULL) {
        /* if there exists an cache node with the same aboslute_uri 
         * delete the old cache object and update it using the new one*/
        delete_cache_node(cache_node);
    }

    /* inserting cache_node to head */
    if ((cache_node = (CacheNode *) malloc(sizeof(CacheNode))) == NULL) {
        /* if malloc for the cachenode fails, give up and return
         * without exiting the program */
        unix_error_non_exit("malloc for cache error");
        V(&writer_mutex);
        return;
    }
    /* set the time info */
    cache_node -> timestamp = time(NULL);
    /* cleaning up pointers */
    cache_node -> next = cache_head;
    cache_node -> prev = NULL;
    if (cache_node -> next) {
        cache_node -> next -> prev = cache_node;
    }
    cache_head = cache_node;

    /* set the actual content and absolute_uri for the cache node */
    cache_node -> absolute_uri = absolute_uri;
    cache_node -> content = content;
    cache_node -> size = size;
    /* release writer lock */
    V(&writer_mutex);
}
