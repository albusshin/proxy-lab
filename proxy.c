#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_connection_hdr = "Proxy-Connection: close\r\n";

typedef struct proxy_info_type{
    int fd;
    char *hostname;
    char *port;
    char *uri;
    rio_t *p_server_rio;
} PI_t;

/*****************Helpers*******************/
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg);
void serve_proxy(PI_t *proxy_info);
/*****************Helpers*******************/

void serve_proxy(PI_t *proxy_info) {
    rio_t rio;
    int clientfd;
    char buf[MAXLINE];
    char key_val_buf[MAXLINE];

    int fd = proxy_info -> fd;
    char *hostname = proxy_info -> hostname;
    char *port = proxy_info -> port;
    char *uri = proxy_info -> uri;
    rio_t *p_server_rio = proxy_info -> p_server_rio;

    free(proxy_info);

    printf("hostname: %s, port: %s\n, uri: %s\n", hostname, port, uri);
    clientfd = Open_clientfd(hostname, port);
    Rio_readinitb(&rio, clientfd);

    /* trasmit first line of request */
    //strcpy(buf, "GET http://www.example.com HTTP/1.0\r\n\r\n");
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
	    printf("%s", buf);
    }
    /* host header */
    if (!host_set) {
        sprintf(buf, "Host: %s", hostname);
        Rio_writen(clientfd, buf, strlen(buf));
	    printf("%s", buf);
    }
    /* user agent header */
    Rio_writen(clientfd, (char *) user_agent_hdr, strlen(user_agent_hdr));
	printf("%s", user_agent_hdr);
    /* connection header */
    Rio_writen(clientfd, (char *) connection_hdr, strlen(connection_hdr));
	printf("%s", connection_hdr);
    /* proxy connection header */
    Rio_writen(clientfd, (char *) proxy_connection_hdr, strlen(proxy_connection_hdr));
	printf("%s", proxy_connection_hdr);

    size_t s = 0;
    while ((s = Rio_readnb(&rio, buf, MAXLINE)) && s != -1) {
        Rio_writen(fd, buf, s);
        Fputs(buf, stdout);
    }

    Fputs("Closed.", stdout);
    Close(clientfd);

    free(hostname);
    free(port);
    free(uri);
    free(p_server_rio);

    return;
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

    char *hostname = (char *) malloc(MAXLINE);
    char *port = (char *) malloc(MAXLINE);
    char *uri = (char *) malloc(MAXLINE);
    rio_t *p_server_rio = (rio_t *) malloc(sizeof(rio_t));
    Rio_readinitb(p_server_rio, fd);
    if (!Rio_readlineb(p_server_rio, buf, MAXLINE))
        return;

    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, requestUri, version);
    if (strcasecmp(method, "GET")) {
        clienterror(fd, method, "501", "Not Implemented",
                    "Proxy  does not implement this method");
        return;
    }
    //TODO clean memory in each thread
    parseUri(requestUri, hostname, port, uri);

    PI_t *proxy_info = malloc(sizeof(PI_t));

    proxy_info -> fd = fd;
    proxy_info -> hostname = hostname;
    proxy_info -> port = port;
    proxy_info -> uri = uri;
    proxy_info -> p_server_rio = p_server_rio;

	serve_proxy(proxy_info);
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
