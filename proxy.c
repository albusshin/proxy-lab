#include <stdio.h>
#include "csapp.h"

//TODO handle interrupts and crashes and SIGPIPE handler
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";

typedef struct proxy_info_type {
    int fd;
    char *hostname;
    char *port;
    char *uri;
    rio_t *p_server_rio;
} ProxyInfo;

typedef struct cache_node_type {
    char *absolute_uri;
    char *content;
    size_t size;
    time_t timestamp;
    struct cache_node_type *next;
    struct cache_node_type *prev;
} CacheNode;

CacheNode *cache_head = NULL;
CacheNode *cache_tail = NULL;

size_t cache_size;
sem_t cache_mutex, cache_w;
int reader_count;

CacheNode *find_cache_node(char *absolute_uri) {
    CacheNode *p = cache_head;
    while (p) {
        if (!strcmp(absolute_uri, p -> absolute_uri)) {
            return p;
        }
        p = p -> next;
    }
    return NULL;
}

CacheNode *get_cache(char *absolute_uri) {
    P(&cache_mutex);
    reader_count++;
    if (reader_count == 1) { /* First in */
        P(&cache_w);
    }
    V(&cache_mutex);

    CacheNode * ret = find_cache_node(absolute_uri);
    if (ret) {
        ret -> timestamp = time(NULL);
    }

    P(&cache_mutex);
    reader_count--;
    if (reader_count == 0) { /* Last out */
        V(&cache_w);
    }
    V(&cache_mutex);
    return ret;
}

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
    else {
        /* cache_node is tail */
        cache_tail = cache_tail -> prev;
    }
    free(cache_node -> absolute_uri);
    free(cache_node -> content);
    free(cache_node);
}

void evict_cache() {
    CacheNode *p, *to_evict = cache_head;
    for (p = cache_head; p; p = p -> next) {
        if (p -> timestamp < to_evict -> timestamp) {
            to_evict = p;
        }
    }
    printf("Cache evict, URI:%s\n", to_evict -> absolute_uri);
    cache_size -= to_evict -> size;
    delete_cache_node(to_evict);
}

//TODO deal with all malloc with smaller case 'm''s, and other stuff that would crash the proxy.
void put_cache(char *absolute_uri, char *content, size_t size) {
    P(&cache_w);
    cache_size += size;
    while (cache_size > MAX_CACHE_SIZE) {
        evict_cache();
    }
    CacheNode *cache_node;
    if ((cache_node = find_cache_node(absolute_uri)) != NULL) {
        delete_cache_node(cache_node);
    }

    /* Inserting cache_node to head */
    cache_node = malloc(sizeof(CacheNode));
    //TODO update time() to be fail safe
    cache_node -> timestamp = time(NULL);
    cache_node -> next = cache_head;
    cache_node -> prev = NULL;
    if (cache_node -> next) {
        cache_node -> next -> prev = cache_node;
    }
    else {
        cache_tail = cache_node;
    }
    cache_head = cache_node;

    cache_node -> absolute_uri = absolute_uri;
    cache_node -> content = content;
    cache_node -> size = size;
    V(&cache_w);
}

/*****************Helpers*******************/
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg);
void serve_proxy(ProxyInfo *proxy_info);
/*****************Helpers*******************/

void serve_proxy(ProxyInfo *proxy_info) {
    rio_t rio;
    int clientfd;
    char buf[MAXLINE];
    char key_val_buf[MAXLINE];

    int fd = proxy_info -> fd;
    char *hostname = proxy_info -> hostname;
    char *port = proxy_info -> port;
    char *uri = proxy_info -> uri;
    rio_t *p_server_rio = proxy_info -> p_server_rio;

    char *cache_absolute_uri = (char *) malloc(MAXLINE);
    sprintf(cache_absolute_uri, "%s:%s%s", hostname, port, uri);

    CacheNode *cache_node;
    if ((cache_node = get_cache(cache_absolute_uri)) != NULL) {
        printf("Cache hit, URI: %s\n", cache_node -> absolute_uri);
        Rio_writen(fd, cache_node -> content, cache_node -> size);
        return;
    }

    clientfd = Open_clientfd(hostname, port);
    Rio_readinitb(&rio, clientfd);

    /* trasmit first line of request */
    sprintf(buf, "GET %s HTTP/1.0\r\n", uri);
    Rio_writen(clientfd, buf, strlen(buf));

    /* read request headers */
    int host_set = 0;
    while(strcmp(buf, "\r\n")) {          //line:netp:readhdrs:checkterm
	    Rio_readlineb(p_server_rio, buf, MAXLINE);
        strcpy(key_val_buf, buf);
        //TODO deal with malformed headers
        char *p_split;
        if ((p_split = strstr(key_val_buf, ":")) != NULL) {
            *p_split = '\0';
            char *keybuf = key_val_buf;

            if (!strcasecmp("Host", keybuf)) {
                host_set = 1;
            }
            if (!strcasecmp("User-Agent", keybuf)) {
                continue;
            }
            if (!strcasecmp("Connection", keybuf)) {
                continue;
            }
            if (!strcasecmp("Proxy-Connection", keybuf)) {
                continue;
            }
        }

        Rio_writen(clientfd, buf, strlen(buf));
    }
    /* host header */
    if (!host_set) {
        sprintf(buf, "Host: %s", hostname);
        Rio_writen(clientfd, buf, strlen(buf));
    }
    /* user agent header */
    Rio_writen(clientfd, (char *) user_agent_hdr, strlen(user_agent_hdr));
    /* connection header */
    Rio_writen(clientfd, (char *) connection_hdr, strlen(connection_hdr));
    /* proxy connection header */
    Rio_writen(clientfd, (char *) proxy_connection_hdr, strlen(proxy_connection_hdr));

    char *cache_content = (char *) malloc(MAX_OBJECT_SIZE);
    char *cachep = cache_content;
    // TODO use strncpys
    // TODO try to exploit your own program! It'll be fun.
    size_t s = 0;
    size_t cache_object_size = 0;
    while ((s = Rio_readnb(&rio, buf, MAXLINE)) && s != -1) {
        Rio_writen(fd, buf, s);
        cache_object_size += s;
        if (cache_object_size <= MAX_OBJECT_SIZE) {
            memcpy(cachep, buf, s);
            cachep += s;
        }
    }
    if (cache_object_size <= MAX_OBJECT_SIZE) {
        put_cache(cache_absolute_uri, cache_content, cache_object_size);
    }
    Close(clientfd);
}

void parseUri(char *requestUri, char *hostname, char *port, char *uri) {
    char *p;

    strcpy(hostname, requestUri);
    /* Parse hostname */
    //TODO DRY
    //TODO Magic #
    if (strstr(hostname, "http://")) {
        strcpy(hostname, hostname + strlen("http://"));
    }
    if ((p = strstr(hostname, ":")) != NULL) {
        *p = '\0';
    }
    if ((p = strstr(hostname, "/")) != NULL) {
        *p = '\0';
    }

    /* Parse port */
    strcpy(port, requestUri);
    if (strstr(port, "http://")) {
        strcpy(port, port + strlen("http://"));
    }

    if ((p = strstr(port, "/")) != NULL) {
        *p = '\0';
    }

    if ((p = strstr(port, ":")) != NULL) {
        strcpy(port, p + 1);

        int port_num = atoi(port);
        //TODO magic #
        if (port_num < 0 || port_num > 65535) {
            strcpy(port, "80");
        }
    }
    else {
        strcpy(port, "80");
    }

    /* Parse uri */
    strcpy(uri, requestUri);
    if (strstr(uri, "http://")) {
        strcpy(uri, uri + strlen("http://"));
    }
    if ((p = strstr(uri, "/")) != NULL) {
        uri = strcpy(uri, p);
    }
    else {
        strcpy(uri, "/");
    }
}

void doit(int fd) {
    char buf[MAXLINE];
    char method[MAXLINE];
    char requestUri[MAXLINE];
    char version[MAXLINE];

    char hostname[MAXLINE];
    char port[MAXLINE];
    char uri[MAXLINE];
    rio_t server_rio;
    Rio_readinitb(&server_rio, fd);
    if (!Rio_readlineb(&server_rio, buf, MAXLINE))
        return;

    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, requestUri, version);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not Implemented",
                    "Proxy  does not implement this method");
        printf("Rejected method %s\n", method);
        return;
    }
    //TODO clean memory in each thread
    parseUri(requestUri, hostname, port, uri);

    ProxyInfo proxy_info;

    proxy_info.fd = fd;
    proxy_info.hostname = hostname;
    proxy_info.port = port;
    proxy_info.uri = uri;
    proxy_info.p_server_rio = &server_rio;

	serve_proxy(&proxy_info);
	Close(fd);
}

//TODO manage failures
void *thread(void *p_fd) {
    Pthread_detach(Pthread_self());
    doit(*((int *) p_fd));
    return NULL;
}

int
main(int argc, char **argv) {
    int listenfd;
    int *connfdp;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    V(&cache_mutex);
    V(&cache_w);
    pthread_t tid;

    /* Check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1) {
	    clientlen = sizeof(clientaddr);
	    //doit(connfd);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        Pthread_create(&tid, NULL, thread, connfdp);
    }
    return 0;
}

















/* $begin clienterror */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
/* $end clienterror */
