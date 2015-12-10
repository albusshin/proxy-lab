/*
 * cache.c - the proxy cache implementation
 *
 * Author: Tian Xin
 * Andrew ID: txin
 *
 * This implementation of cache uses a relaxed LRU eviction policy.
 * It utilizes the structure of linked list and in each of the list node,
 * there are information about the cached web objects. Also, inside each
 * node, the timestamp of its last access is stored. So when eviction has
 * to happen, the oldest object with the smallest timestamp is deleted.
 *
 * Reader - writer lock is implemented in this cache to support concurrency.
 * There can be multiple readers reading simultaneously and there can be only
 * one client writing or updating the structure of the linkedlist at a time.
 * The implementation is in favor of the readers and starves the writers.
 */
#include "cache.h"
#include "csapp.h"
#include "proxylib.h"

CacheNode *cache_head = NULL;   /* the cache linked list head */
size_t cache_size = 0;          /* the current cache size */
sem_t reader_count_mutex;       /* the reader_count lock */
sem_t writer_mutex;             /* the writer semaphore */
int reader_count = 0;           /* the current reader count */

/*
 * init_cache - initialize cache operations 
 */
void init_cache() {
    /* set reader_count_mutex = 1 */
    V(&reader_count_mutex);
    /* set writer_mutex = 1 */
    V(&writer_mutex);
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
 * evict_cache - cache evict method
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
 * get_cache - cache get method
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
        printf("Cache hit, timestamp: %lu\n", 
                (unsigned long) ret -> timestamp);
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
 * put_cache - cache put method
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

