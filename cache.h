/*
 * cache.h - type declarations and function declarations for the proxy cache
 *
 * Author: Tian Xin
 * Andrew ID: txin
 */
#include <stdlib.h>

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* the cache object linked list node */
typedef struct cache_node_type {
    char *absolute_uri;
    char *content;
    size_t size;
    time_t timestamp;
    struct cache_node_type *next;
    struct cache_node_type *prev;
} CacheNode;

void init_cache();
CacheNode *find_cache_node(char *absolute_uri);
void delete_cache_node(CacheNode *cache_node);
void evict_cache();
CacheNode *get_cache(char *absolute_uri);
void put_cache(char *absolute_uri, char *content, size_t size);

